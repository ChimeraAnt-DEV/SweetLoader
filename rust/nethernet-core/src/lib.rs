//! `nethernet-core` — the parts of the NetherNet transport that can be tested without a network.
//!
//! The transport itself (`nethernet-jni`) opens sockets and talks to a signaling service, which
//! makes it the hardest part of the client to test and the most valuable part to keep small. So the
//! protocol is split out here: signaling message parsing, the reconnect backoff policy, and the
//! packet framing layer are all pure functions over byte slices, exercised by unit tests on the
//! build host with no socket in sight.
//!
//! What remains in the JNI crate is the part that genuinely needs a device: the socket, the TLS
//! handshake, and the JNI marshalling.

pub mod backoff;
pub mod framing;
pub mod session;
pub mod signaling;

pub use backoff::BackoffPolicy;
pub use framing::{
    decode_frame, Frame, FrameError, FrameKind, FrameReader, ReorderBuffer, MAX_FRAME_BYTES,
};
pub use session::{Session, TransportError, TransportState};
pub use signaling::{parse_server_signal, Signal, SignalError, PROTOCOL_VERSION};
