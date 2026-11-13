//! Framing tests: every byte-level way a peer can be wrong.
//!
//! The framing layer sits between a hostile network and the game thread, so the tests here are
//! deliberately adversarial: truncated frames, absurd length prefixes, control frames with payloads,
//! duplicates, reordering and sequence rollover. Each one feeds exact bytes rather than calling a
//! helper, so a change to the header layout cannot pass by accident.

use nethernet_core::framing::{
    decode_frame, Frame, FrameError, FrameKind, FrameReader, ReorderBuffer, HEADER_BYTES,
    MAX_FRAME_BYTES,
};

fn header(body_len: u32, flags: u8, seq: u32) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&body_len.to_be_bytes());
    v.push(flags);
    v.extend_from_slice(&seq.to_be_bytes());
    v
}

#[test]
fn a_data_frame_round_trips() {
    let frame = Frame::data(42, b"hello".to_vec());
    let bytes = frame.encode().unwrap();
    assert_eq!(bytes.len(), HEADER_BYTES + 5);
    assert_eq!(decode_frame(&bytes).unwrap(), frame);
}

#[test]
fn an_empty_payload_is_a_valid_data_frame() {
    let frame = Frame::data(1, Vec::new());
    let bytes = frame.encode().unwrap();
    assert_eq!(decode_frame(&bytes).unwrap(), frame);
}

#[test]
fn the_length_prefix_counts_everything_after_itself() {
    let bytes = Frame::data(7, b"abc".to_vec()).encode().unwrap();
    let declared = u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) as usize;
    assert_eq!(declared, bytes.len() - 4);
}

#[test]
fn the_header_is_big_endian() {
    let bytes = Frame::data(0x0102_0304, b"".to_vec()).encode().unwrap();
    // 5 bytes of flags+seq, so the length is 5 and the sequence follows.
    assert_eq!(&bytes[0..4], &5u32.to_be_bytes());
    assert_eq!(&bytes[5..9], &0x0102_0304u32.to_be_bytes());
}

#[test]
fn the_reliable_bit_is_separate_from_the_kind() {
    let reliable = Frame::data(1, b"x".to_vec()).encode().unwrap();
    assert_eq!(reliable[4], 0x80, "reliable data must set the top bit");
    let unreliable =
        Frame { kind: FrameKind::Data, sequence: 1, reliable: false, payload: b"x".to_vec() }
            .encode()
            .unwrap();
    assert_eq!(unreliable[4], 0x00);
    assert!(!decode_frame(&unreliable).unwrap().reliable);
}

#[test]
fn ack_nack_and_keepalive_round_trip() {
    for frame in [Frame::ack(3), Frame::nack(4), Frame::keepalive(5)] {
        let bytes = frame.encode().unwrap();
        let decoded = decode_frame(&bytes).unwrap();
        assert_eq!(decoded, frame);
        assert!(decoded.kind.is_control());
    }
}

#[test]
fn a_frame_shorter_than_the_header_is_refused() {
    assert_eq!(decode_frame(&[0u8; 4]).unwrap_err(), FrameError::LengthTooSmall(4));
}

#[test]
fn a_length_prefix_below_the_fixed_body_is_refused() {
    let bytes = header(4, 0, 0);
    assert_eq!(decode_frame(&bytes).unwrap_err(), FrameError::LengthTooSmall(4));
}

#[test]
fn a_length_prefix_above_the_cap_is_refused() {
    let bytes = header((MAX_FRAME_BYTES + 1) as u32 + 5, 0, 0);
    assert_eq!(
        decode_frame(&bytes).unwrap_err(),
        FrameError::FrameTooLarge(MAX_FRAME_BYTES + 1)
    );
}

#[test]
fn a_payload_exactly_at_the_cap_is_accepted() {
    let frame = Frame::data(1, vec![0u8; MAX_FRAME_BYTES]);
    let bytes = frame.encode().unwrap();
    assert_eq!(decode_frame(&bytes).unwrap().payload.len(), MAX_FRAME_BYTES);
}

#[test]
fn encoding_an_oversized_payload_is_refused_rather_than_truncated() {
    let frame = Frame::data(1, vec![0u8; MAX_FRAME_BYTES + 1]);
    assert_eq!(frame.encode().unwrap_err(), FrameError::PayloadTooLarge(MAX_FRAME_BYTES + 1));
}

#[test]
fn an_unknown_kind_byte_is_refused_with_its_value() {
    let bytes = header(5, 0x7F, 0);
    assert_eq!(decode_frame(&bytes).unwrap_err(), FrameError::UnknownKind(0x7F));
}

#[test]
fn a_control_frame_carrying_a_payload_is_refused() {
    // kind 1 is Ack; give it a one-byte payload.
    let mut bytes = header(6, 1, 0);
    bytes.push(0xAA);
    assert_eq!(decode_frame(&bytes).unwrap_err(), FrameError::ControlWithPayload(FrameKind::Ack));
}

#[test]
fn the_reader_yields_nothing_until_a_frame_is_complete() {
    let bytes = Frame::data(1, b"payload".to_vec()).encode().unwrap();
    let mut reader = FrameReader::new();
    for byte in &bytes[..bytes.len() - 1] {
        reader.push(&[*byte]).unwrap();
        assert!(reader.next_frame().is_none(), "a partial frame must not be delivered");
    }
    reader.push(&bytes[bytes.len() - 1..]).unwrap();
    assert_eq!(reader.next_frame().unwrap().unwrap().payload, b"payload");
    assert_eq!(reader.buffered(), 0);
}

#[test]
fn the_reader_handles_arbitrary_chunk_boundaries() {
    let a = Frame::data(1, b"first".to_vec()).encode().unwrap();
    let b = Frame::ack(2).encode().unwrap();
    let c = Frame::keepalive(3).encode().unwrap();
    let mut stream = Vec::new();
    stream.extend_from_slice(&a);
    stream.extend_from_slice(&b);
    stream.extend_from_slice(&c);

    // Feed the whole stream one byte at a time; the three frames must come out in order.
    let mut reader = FrameReader::new();
    let mut frames = Vec::new();
    for byte in stream {
        reader.push(&[byte]).unwrap();
        while let Some(frame) = reader.next_frame() {
            frames.push(frame.unwrap());
        }
    }
    assert_eq!(frames.len(), 3);
    assert_eq!(frames[0].payload, b"first");
    assert_eq!(frames[1].kind, FrameKind::Ack);
    assert_eq!(frames[2].kind, FrameKind::Keepalive);
}

#[test]
fn the_reader_reports_a_bad_length_prefix_and_keeps_making_progress() {
    let mut reader = FrameReader::new();
    // A length prefix of 1 is impossible: the fixed body is 5 bytes.
    reader.push(&header(1, 0, 0)).unwrap();
    assert_eq!(reader.next_frame().unwrap().unwrap_err(), FrameError::LengthTooSmall(1));
    // The bad header was consumed, so a following valid frame still decodes.
    reader.push(&Frame::data(9, b"ok".to_vec()).encode().unwrap()).unwrap();
    assert_eq!(reader.next_frame().unwrap().unwrap().payload, b"ok");
}

#[test]
fn the_reader_refuses_a_peer_that_never_completes_a_frame() {
    let mut reader = FrameReader::new();
    // Claim the largest legal frame, then keep sending filler without ever finishing it.
    reader.push(&header((MAX_FRAME_BYTES + 5) as u32, 0, 0)).unwrap();
    let chunk = vec![0u8; 64 * 1024];
    let mut refused = false;
    for _ in 0..64 {
        if reader.push(&chunk).is_err() {
            refused = true;
            break;
        }
    }
    assert!(refused, "the reader must refuse an unbounded frame, not grow forever");
}

#[test]
fn a_duplicate_sequence_number_is_rejected() {
    let mut reorder = ReorderBuffer::new();
    assert!(reorder.accept(1));
    assert!(!reorder.accept(1), "a duplicate must not be delivered twice");
    assert!(reorder.accept(2));
    assert!(!reorder.accept(2));
}

#[test]
fn an_out_of_order_older_frame_is_rejected() {
    let mut reorder = ReorderBuffer::new();
    assert!(reorder.accept(10));
    assert!(!reorder.accept(9), "an older frame must not be delivered late");
    assert!(reorder.accept(11));
}

#[test]
fn the_reorder_buffer_reports_its_high_water_mark() {
    let mut reorder = ReorderBuffer::new();
    assert_eq!(reorder.highest(), None);
    reorder.accept(5);
    assert_eq!(reorder.highest(), Some(5));
    reorder.accept(8);
    assert_eq!(reorder.highest(), Some(8));
    reorder.accept(6);
    assert_eq!(reorder.highest(), Some(8), "a rejected frame must not lower the mark");
}

#[test]
fn a_sequence_rollover_is_still_ordered() {
    let mut reorder = ReorderBuffer::new();
    assert!(reorder.accept(u32::MAX - 1));
    assert!(reorder.accept(u32::MAX));
    assert!(reorder.accept(0), "the sequence must wrap, not reset");
    assert!(reorder.accept(1));
    assert!(!reorder.accept(u32::MAX), "the old high frame must still be rejected");
}

#[test]
fn reset_clears_the_high_water_mark() {
    let mut reorder = ReorderBuffer::new();
    reorder.accept(100);
    reorder.reset();
    assert_eq!(reorder.highest(), None);
    assert!(reorder.accept(1), "a new connection restarts the sequence space");
}

#[test]
fn a_frame_error_never_says_panic() {
    let cases = [
        FrameError::FrameTooLarge(1),
        FrameError::LengthTooSmall(1),
        FrameError::UnknownKind(9),
        FrameError::ControlWithPayload(FrameKind::Ack),
        FrameError::PayloadTooLarge(1),
    ];
    for case in cases {
        let text = case.to_string();
        assert!(!text.contains("panic"), "{text}");
    }
}
