//! The NetherNet signaling protocol: what a client says to the signaling service, and what the
//! service says back.
//!
//! This module is pure. It parses and builds messages and nothing else, so every branch a malformed
//! or hostile server can reach is exercised by a unit test rather than by a live connection. The
//! transport (`nethernet-jni`) only moves these bytes.
//!
//! The protocol is a small JSON request/response exchange over a WebSocket. Because the service is
//! the only party that can be wrong here, parsing is deliberately total: every field is optional at
//! the type level and validated into a [`Signal`] or a [`SignalError`]. A message this code does not
//! understand is a `SignalError::Unsupported`, never a panic and never a silent no-op — a client
//! that ignores an unknown signal type is a client that hangs.

use serde::{Deserialize, Serialize};

/// The protocol revision this client speaks. Sent on connect so the service can refuse an old client
/// with a real error instead of failing at the first unrecognised message.
pub const PROTOCOL_VERSION: u32 = 1;

/// The largest signaling frame this client will read. The real messages are a few hundred bytes; the
/// cap exists so a hostile or broken server cannot make the client allocate without bound.
pub const MAX_SIGNAL_FRAME_BYTES: usize = 64 * 1024;

/// What the client sends to the service.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ClientSignal {
    /// First message on every connection.
    Hello {
        protocol: u32,
        /// The client's own network id, as a decimal string.
        network_id: String,
    },
    /// Request a session with a peer.
    Connect {
        /// The peer's network id, as a decimal string.
        peer_network_id: String,
    },
    /// A negotiated ICE offer/answer payload, passed through opaquely.
    Candidate {
        /// The session this candidate belongs to.
        session: String,
        /// The serialised candidate. The client does not interpret it.
        payload: String,
    },
    /// Tear down a session.
    Disconnect { session: String },
}

/// What the service sends to the client.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ServerSignal {
    /// The service accepted the handshake.
    Welcome {
        protocol: u32,
        /// The session the service assigned.
        session: String,
    },
    /// The service is asking the client to connect to a peer.
    Connect {
        session: String,
        peer_network_id: String,
    },
    /// A negotiated ICE payload for a session.
    Candidate { session: String, payload: String },
    /// The peer or the service ended the session.
    Disconnect {
        session: String,
        /// Why, in the service's own words. Shown to the user when it is the only explanation.
        #[serde(default)]
        reason: String,
    },
    /// The service is unhealthy or the client is not allowed to connect yet.
    Error {
        code: String,
        #[serde(default)]
        message: String,
    },
}

/// A parsed server message, or the reason it could not be used.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Signal {
    Welcome {
        protocol: u32,
        session: String,
    },
    Connect {
        session: String,
        peer_network_id: String,
    },
    Candidate {
        session: String,
        payload: String,
    },
    Disconnect {
        session: String,
        reason: String,
    },
    /// The service reported an error. Distinct from a parse failure: this one is a real answer.
    ServiceError {
        code: String,
        message: String,
    },
}

/// Why a server message could not be used.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SignalError {
    /// The frame was not valid JSON.
    MalformedJson(String),
    /// The frame was JSON but not a message this protocol defines.
    Unsupported(String),
    /// The frame was a known message with a missing or empty required field.
    MissingField(&'static str),
    /// The frame exceeded [`MAX_SIGNAL_FRAME_BYTES`].
    TooLarge(usize),
    /// The service speaks a protocol revision this client does not.
    ProtocolMismatch { client: u32, server: u32 },
}

impl std::fmt::Display for SignalError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::MalformedJson(d) => write!(f, "malformed signaling frame: {d}"),
            Self::Unsupported(t) => write!(f, "unsupported signaling message: {t}"),
            Self::MissingField(field) => write!(f, "signaling message is missing {field}"),
            Self::TooLarge(n) => write!(f, "signaling frame of {n} bytes exceeds the cap"),
            Self::ProtocolMismatch { client, server } => {
                write!(
                    f,
                    "signaling protocol mismatch: client {client}, server {server}"
                )
            }
        }
    }
}

impl std::error::Error for SignalError {}

/// Parses one server frame.
///
/// Total by construction: every failure is a [`SignalError`], so the transport thread never panics on
/// hostile input.
pub fn parse_server_signal(frame: &[u8]) -> Result<Signal, SignalError> {
    if frame.len() > MAX_SIGNAL_FRAME_BYTES {
        return Err(SignalError::TooLarge(frame.len()));
    }
    let text = std::str::from_utf8(frame).map_err(|e| SignalError::MalformedJson(e.to_string()))?;
    // The message type is read before the full deserialise so an unknown type reports as
    // `Unsupported` with its name, which is what makes a protocol drift diagnosable from a log.
    let probe: serde_json::Value =
        serde_json::from_str(text).map_err(|e| SignalError::MalformedJson(e.to_string()))?;
    let kind = probe
        .get("type")
        .and_then(|v| v.as_str())
        .ok_or(SignalError::MissingField("type"))?
        .to_string();

    let message: ServerSignal =
        serde_json::from_value(probe).map_err(|_| SignalError::Unsupported(kind.clone()))?;

    let signal = match message {
        ServerSignal::Welcome { protocol, session } => {
            if session.is_empty() {
                return Err(SignalError::MissingField("session"));
            }
            if protocol != PROTOCOL_VERSION {
                return Err(SignalError::ProtocolMismatch {
                    client: PROTOCOL_VERSION,
                    server: protocol,
                });
            }
            Signal::Welcome { protocol, session }
        }
        ServerSignal::Connect {
            session,
            peer_network_id,
        } => {
            if session.is_empty() {
                return Err(SignalError::MissingField("session"));
            }
            if peer_network_id.is_empty() {
                return Err(SignalError::MissingField("peer_network_id"));
            }
            Signal::Connect {
                session,
                peer_network_id,
            }
        }
        ServerSignal::Candidate { session, payload } => {
            if session.is_empty() {
                return Err(SignalError::MissingField("session"));
            }
            if payload.is_empty() {
                return Err(SignalError::MissingField("payload"));
            }
            Signal::Candidate { session, payload }
        }
        ServerSignal::Disconnect { session, reason } => {
            if session.is_empty() {
                return Err(SignalError::MissingField("session"));
            }
            Signal::Disconnect { session, reason }
        }
        ServerSignal::Error { code, message } => {
            if code.is_empty() {
                return Err(SignalError::MissingField("code"));
            }
            Signal::ServiceError { code, message }
        }
    };
    Ok(signal)
}

/// Serialises a client message. Infallible for every variant the client constructs.
pub fn encode_client_signal(signal: &ClientSignal) -> String {
    serde_json::to_string(signal).expect("client signals always serialise")
}

/// The hello this client sends first.
pub fn hello(network_id: &str) -> ClientSignal {
    ClientSignal::Hello {
        protocol: PROTOCOL_VERSION,
        network_id: network_id.to_string(),
    }
}

/// Builds a connect request.
pub fn connect(peer_network_id: &str) -> ClientSignal {
    ClientSignal::Connect {
        peer_network_id: peer_network_id.to_string(),
    }
}

/// Builds a candidate relay message.
pub fn candidate(session: &str, payload: &str) -> ClientSignal {
    ClientSignal::Candidate {
        session: session.to_string(),
        payload: payload.to_string(),
    }
}

/// Builds a disconnect.
pub fn disconnect(session: &str) -> ClientSignal {
    ClientSignal::Disconnect {
        session: session.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hello_carries_the_protocol_version() {
        let encoded = encode_client_signal(&hello("1234"));
        assert!(encoded.contains("\"type\":\"hello\""));
        assert!(encoded.contains("\"protocol\":1"));
        assert!(encoded.contains("\"network_id\":\"1234\""));
    }

    #[test]
    fn a_welcome_with_the_matching_protocol_parses() {
        let signal =
            parse_server_signal(br#"{"type":"welcome","protocol":1,"session":"s-1"}"#).unwrap();
        assert_eq!(
            signal,
            Signal::Welcome {
                protocol: 1,
                session: "s-1".into()
            }
        );
    }

    #[test]
    fn a_welcome_with_a_future_protocol_is_a_mismatch_not_a_hang() {
        let err =
            parse_server_signal(br#"{"type":"welcome","protocol":2,"session":"s"}"#).unwrap_err();
        assert_eq!(
            err,
            SignalError::ProtocolMismatch {
                client: 1,
                server: 2
            }
        );
    }

    #[test]
    fn an_unknown_message_type_is_reported_with_its_name() {
        let err = parse_server_signal(br#"{"type":"teleport","x":1}"#).unwrap_err();
        assert_eq!(err, SignalError::Unsupported("teleport".into()));
    }

    #[test]
    fn invalid_json_is_a_parse_error_not_a_panic() {
        let err = parse_server_signal(b"{not json").unwrap_err();
        assert!(matches!(err, SignalError::MalformedJson(_)));
    }

    #[test]
    fn non_utf8_is_a_parse_error() {
        let err = parse_server_signal(&[0xff, 0xfe, 0xfd]).unwrap_err();
        assert!(matches!(err, SignalError::MalformedJson(_)));
    }

    #[test]
    fn an_oversized_frame_is_refused_before_parsing() {
        let frame = vec![b'x'; MAX_SIGNAL_FRAME_BYTES + 1];
        assert_eq!(
            parse_server_signal(&frame).unwrap_err(),
            SignalError::TooLarge(MAX_SIGNAL_FRAME_BYTES + 1)
        );
    }

    #[test]
    fn a_frame_at_exactly_the_cap_is_accepted_for_parsing() {
        // Valid JSON padded to exactly the cap: the cap must not be off by one.
        let mut body = String::from(r#"{"type":"candidate","session":"s","payload":""#);
        let padding = MAX_SIGNAL_FRAME_BYTES - body.len() - 2;
        body.push_str(&"a".repeat(padding));
        body.push_str("\"}");
        assert_eq!(body.len(), MAX_SIGNAL_FRAME_BYTES);
        let signal = parse_server_signal(body.as_bytes()).unwrap();
        assert!(matches!(signal, Signal::Candidate { .. }));
    }

    #[test]
    fn a_missing_type_is_a_missing_field() {
        assert_eq!(
            parse_server_signal(br#"{"session":"s"}"#).unwrap_err(),
            SignalError::MissingField("type")
        );
    }

    #[test]
    fn an_empty_session_is_rejected_for_every_session_message() {
        for frame in [
            &br#"{"type":"connect","session":"","peer_network_id":"9"}"#[..],
            &br#"{"type":"candidate","session":"","payload":"p"}"#[..],
            &br#"{"type":"disconnect","session":""}"#[..],
            &br#"{"type":"welcome","protocol":1,"session":""}"#[..],
        ] {
            assert_eq!(
                parse_server_signal(frame).unwrap_err(),
                SignalError::MissingField("session"),
                "frame {}",
                String::from_utf8_lossy(frame)
            );
        }
    }

    #[test]
    fn an_empty_peer_id_is_rejected() {
        assert_eq!(
            parse_server_signal(br#"{"type":"connect","session":"s","peer_network_id":""}"#)
                .unwrap_err(),
            SignalError::MissingField("peer_network_id")
        );
    }

    #[test]
    fn an_empty_candidate_payload_is_rejected() {
        assert_eq!(
            parse_server_signal(br#"{"type":"candidate","session":"s","payload":""}"#).unwrap_err(),
            SignalError::MissingField("payload")
        );
    }

    #[test]
    fn an_empty_error_code_is_rejected() {
        assert_eq!(
            parse_server_signal(br#"{"type":"error","code":""}"#).unwrap_err(),
            SignalError::MissingField("code")
        );
    }

    #[test]
    fn a_disconnect_reason_is_optional_and_defaults_to_empty() {
        let signal = parse_server_signal(br#"{"type":"disconnect","session":"s"}"#).unwrap();
        assert_eq!(
            signal,
            Signal::Disconnect {
                session: "s".into(),
                reason: String::new()
            }
        );
    }

    #[test]
    fn a_service_error_is_a_real_answer_not_a_parse_failure() {
        let signal =
            parse_server_signal(br#"{"type":"error","code":"busy","message":"try later"}"#)
                .unwrap();
        assert_eq!(
            signal,
            Signal::ServiceError {
                code: "busy".into(),
                message: "try later".into()
            }
        );
    }

    #[test]
    fn a_wrongly_typed_field_is_unsupported_not_a_panic() {
        // `session` is a number where the protocol says string.
        let err =
            parse_server_signal(br#"{"type":"welcome","protocol":1,"session":7}"#).unwrap_err();
        assert_eq!(err, SignalError::Unsupported("welcome".into()));
    }

    #[test]
    fn every_client_signal_round_trips_through_json() {
        for signal in [
            hello("1"),
            connect("2"),
            candidate("s", "p"),
            disconnect("s"),
        ] {
            let encoded = encode_client_signal(&signal);
            let decoded: ClientSignal = serde_json::from_str(&encoded).unwrap();
            assert_eq!(signal, decoded);
        }
    }

    #[test]
    fn the_error_display_never_mentions_a_panic() {
        let cases = [
            SignalError::MalformedJson("x".into()),
            SignalError::Unsupported("t".into()),
            SignalError::MissingField("session"),
            SignalError::TooLarge(10),
            SignalError::ProtocolMismatch {
                client: 1,
                server: 2,
            },
        ];
        for case in cases {
            let text = case.to_string();
            assert!(!text.contains("panic"), "{text}");
            assert!(!text.is_empty());
        }
    }
}
