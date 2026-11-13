//! The packet framing layer: how a stream of bytes becomes messages, and how messages become
//! messages again after the network loses, reorders or duplicates them.
//!
//! This is the part of the transport with the most ways to be wrong, so it is pure: [`FrameReader`]
//! consumes bytes and yields frames, [`Frame::encode`] produces bytes, and neither touches a socket.
//! Every failure a hostile peer can cause — a length prefix larger than the cap, a truncated frame, a
//! duplicate, a reordered packet — has a test that feeds it the exact bytes.
//!
//! The wire format is a 4-byte big-endian length prefix, a 1-byte flags field, a 4-byte sequence
//! number and the payload:
//!
//! ```text
//! 0        4      5        9              9+len
//! +--------+------+--------+---------------+
//! | length | flags| seq    | payload       |
//! +--------+------+--------+---------------+
//! ```
//!
//! `length` counts everything after itself, so the reader knows how many more bytes to wait for.
//! Big-endian because the rest of the Bedrock wire protocol is big-endian and a mixed-endian frame
//! header is a bug waiting for a developer with a hex editor.

/// The largest payload this layer will accept. A Bedrock chunk packet is well under 1 MB; the cap
/// stops a peer from making the reader allocate gigabytes by claiming a huge frame.
pub const MAX_FRAME_BYTES: usize = 1024 * 1024;

/// The fixed header size: 4-byte length, 1-byte flags, 4-byte sequence.
pub const HEADER_BYTES: usize = 9;

/// The message classes this layer distinguishes. The kind is a field in the flags byte rather than a
/// separate byte so a future kind does not change the header size.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameKind {
    /// Application data.
    Data,
    /// Acknowledges a sequence number. Carries no payload.
    Ack,
    /// Asks the peer to resend everything after a sequence number.
    Nack,
    /// A keepalive. Carries no payload and is never retransmitted.
    Keepalive,
    /// The first fragment of a fragmented message.
    FragmentStart,
    /// A middle fragment.
    FragmentContinue,
    /// The last fragment.
    FragmentEnd,
}

impl FrameKind {
    fn to_bits(self) -> u8 {
        match self {
            Self::Data => 0,
            Self::Ack => 1,
            Self::Nack => 2,
            Self::Keepalive => 3,
            Self::FragmentStart => 4,
            Self::FragmentContinue => 5,
            Self::FragmentEnd => 6,
        }
    }

    fn from_bits(bits: u8) -> Option<Self> {
        Some(match bits {
            0 => Self::Data,
            1 => Self::Ack,
            2 => Self::Nack,
            3 => Self::Keepalive,
            4 => Self::FragmentStart,
            5 => Self::FragmentContinue,
            6 => Self::FragmentEnd,
            _ => return None,
        })
    }

    /// True for kinds that never carry a payload.
    pub fn is_control(self) -> bool {
        matches!(self, Self::Ack | Self::Nack | Self::Keepalive)
    }
}

/// One frame.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub kind: FrameKind,
    pub sequence: u32,
    /// True when this frame wants an ack.
    pub reliable: bool,
    pub payload: Vec<u8>,
}

impl Frame {
    /// A reliable data frame.
    pub fn data(sequence: u32, payload: Vec<u8>) -> Self {
        Self { kind: FrameKind::Data, sequence, reliable: true, payload }
    }

    /// An acknowledgement. `sequence` is the number being acknowledged.
    pub fn ack(sequence: u32) -> Self {
        Self { kind: FrameKind::Ack, sequence, reliable: false, payload: Vec::new() }
    }

    /// A negative acknowledgement: "resend from here".
    pub fn nack(sequence: u32) -> Self {
        Self { kind: FrameKind::Nack, sequence, reliable: false, payload: Vec::new() }
    }

    /// A keepalive.
    pub fn keepalive(sequence: u32) -> Self {
        Self { kind: FrameKind::Keepalive, sequence, reliable: false, payload: Vec::new() }
    }

    /// Encodes this frame to bytes. Fails only if the payload exceeds [`MAX_FRAME_BYTES`].
    pub fn encode(&self) -> Result<Vec<u8>, FrameError> {
        if self.payload.len() > MAX_FRAME_BYTES {
            return Err(FrameError::PayloadTooLarge(self.payload.len()));
        }
        let body_len = 5 + self.payload.len(); // flags + seq + payload
        let mut out = Vec::with_capacity(HEADER_BYTES + self.payload.len());
        out.extend_from_slice(&(body_len as u32).to_be_bytes());
        let flags = self.kind.to_bits() | if self.reliable { 0x80 } else { 0 };
        out.push(flags);
        out.extend_from_slice(&self.sequence.to_be_bytes());
        out.extend_from_slice(&self.payload);
        Ok(out)
    }
}

/// Why a frame could not be decoded.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FrameError {
    /// The length prefix claims more than [`MAX_FRAME_BYTES`].
    FrameTooLarge(usize),
    /// The length prefix is smaller than the fixed body it must contain.
    LengthTooSmall(usize),
    /// The flags byte names a kind this version does not define.
    UnknownKind(u8),
    /// A control frame carried a payload.
    ControlWithPayload(FrameKind),
    /// A payload exceeded the cap while encoding.
    PayloadTooLarge(usize),
}

impl std::fmt::Display for FrameError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::FrameTooLarge(n) => write!(f, "frame of {n} bytes exceeds the {MAX_FRAME_BYTES}-byte cap"),
            Self::LengthTooSmall(n) => write!(f, "frame length {n} is below the {HEADER_BYTES}-byte header"),
            Self::UnknownKind(k) => write!(f, "unknown frame kind {k}"),
            Self::ControlWithPayload(k) => write!(f, "control frame {k:?} carried a payload"),
            Self::PayloadTooLarge(n) => write!(f, "payload of {n} bytes exceeds the cap"),
        }
    }
}

impl std::error::Error for FrameError {}

/// Decodes exactly one frame from a complete byte slice.
///
/// Used for tests and for the in-memory path; the streaming path is [`FrameReader`].
pub fn decode_frame(bytes: &[u8]) -> Result<Frame, FrameError> {
    if bytes.len() < HEADER_BYTES {
        return Err(FrameError::LengthTooSmall(bytes.len()));
    }
    let body_len = u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) as usize;
    if body_len < 5 {
        return Err(FrameError::LengthTooSmall(body_len));
    }
    if body_len - 5 > MAX_FRAME_BYTES {
        return Err(FrameError::FrameTooLarge(body_len - 5));
    }
    let flags = bytes[4];
    let kind = FrameKind::from_bits(flags & 0x7F).ok_or(FrameError::UnknownKind(flags & 0x7F))?;
    let reliable = flags & 0x80 != 0;
    let sequence = u32::from_be_bytes([bytes[5], bytes[6], bytes[7], bytes[8]]);
    let payload = bytes[HEADER_BYTES..].to_vec();
    if kind.is_control() && !payload.is_empty() {
        return Err(FrameError::ControlWithPayload(kind));
    }
    Ok(Frame { kind, sequence, reliable, payload })
}

/// A streaming reader that turns arbitrary chunk boundaries into whole frames.
///
/// The reader owns its buffer and never allocates per frame beyond the frame's own payload, which
/// matters because this runs on every inbound packet.
#[derive(Debug, Default)]
pub struct FrameReader {
    buffer: Vec<u8>,
}

impl FrameReader {
    pub fn new() -> Self {
        Self { buffer: Vec::new() }
    }

    /// Bytes currently buffered but not yet a complete frame.
    pub fn buffered(&self) -> usize {
        self.buffer.len()
    }

    /// Appends bytes. Fails on a length prefix that is already invalid, so a hostile peer is rejected
    /// at the first byte rather than after buffering megabytes.
    pub fn push(&mut self, bytes: &[u8]) -> Result<(), FrameError> {
        if self.buffer.len() + bytes.len() > HEADER_BYTES + MAX_FRAME_BYTES + 1024 {
            // A peer that keeps sending without ever completing a frame is either broken or hostile.
            return Err(FrameError::FrameTooLarge(self.buffer.len() + bytes.len()));
        }
        self.buffer.extend_from_slice(bytes);
        Ok(())
    }

    /// Pops the next complete frame, or `None` when more bytes are needed.
    ///
    /// A frame with an invalid header is returned as an error and consumed, so one bad frame does not
    /// wedge the stream forever.
    pub fn next_frame(&mut self) -> Option<Result<Frame, FrameError>> {
        if self.buffer.len() < HEADER_BYTES {
            return None;
        }
        let body_len = u32::from_be_bytes([
            self.buffer[0],
            self.buffer[1],
            self.buffer[2],
            self.buffer[3],
        ]) as usize;
        if body_len < 5 || body_len - 5 > MAX_FRAME_BYTES {
            // Drop the header so the next call makes progress; the caller decides whether to close.
            self.buffer.drain(..HEADER_BYTES);
            let err = if body_len < 5 {
                FrameError::LengthTooSmall(body_len)
            } else {
                FrameError::FrameTooLarge(body_len - 5)
            };
            return Some(Err(err));
        }
        let total = 4 + body_len;
        if self.buffer.len() < total {
            return None;
        }
        let frame_bytes: Vec<u8> = self.buffer.drain(..total).collect();
        Some(decode_frame(&frame_bytes))
    }
}

/// A minimal reorder/duplicate filter for reliable frames.
///
/// Bedrock's transport is UDP underneath, so a reliable stream has to be reconstructed. This tracks
/// the highest sequence number delivered and rejects anything at or below it, which is enough to
/// drop duplicates and late arrivals. It deliberately does not buffer out-of-order frames: the
/// caller resends via NACK, which keeps this structure O(1) and allocation-free.
#[derive(Debug, Default)]
pub struct ReorderBuffer {
    highest_delivered: Option<u32>,
}

impl ReorderBuffer {
    pub fn new() -> Self {
        Self { highest_delivered: None }
    }

    /// True when this sequence number is new and should be delivered.
    ///
    /// Uses wrapping comparison so a sequence number that rolls over `u32::MAX` is still ordered.
    pub fn accept(&mut self, sequence: u32) -> bool {
        match self.highest_delivered {
            None => {
                self.highest_delivered = Some(sequence);
                true
            }
            Some(highest) => {
                let forward = sequence.wrapping_sub(highest);
                // Anything more than half the sequence space ahead is actually behind (wrap).
                if forward == 0 || forward > u32::MAX / 2 {
                    false
                } else {
                    self.highest_delivered = Some(sequence);
                    true
                }
            }
        }
    }

    /// The highest sequence number delivered, or `None` before any frame.
    pub fn highest(&self) -> Option<u32> {
        self.highest_delivered
    }

    /// Resets the buffer, for a new connection.
    pub fn reset(&mut self) {
        self.highest_delivered = None;
    }
}
