class_name FakeSocket
extends RefCounted

## Duck-typed stand-in for WebSocketPeer, matching only the methods
## UniverseClient actually calls. Lets ticket #16's reconnect and
## disconnected-command behavior be unit-tested without a live bridge.

var ready_state: int = WebSocketPeer.STATE_CLOSED
var sent_texts: Array[String] = []
var connect_call_count: int = 0

var _incoming: Array[PackedByteArray] = []

func connect_to_url(_url: String) -> int:
	connect_call_count += 1
	return OK

func poll() -> void:
	pass

func get_ready_state() -> int:
	return ready_state

func get_available_packet_count() -> int:
	return _incoming.size()

func get_packet() -> PackedByteArray:
	return _incoming.pop_front()

func send_text(text: String) -> int:
	sent_texts.append(text)
	return OK

## Test helper, not part of the WebSocketPeer surface: queue a text frame
## as if it had arrived from the server.
func push_incoming(text: String) -> void:
	_incoming.append(text.to_utf8_buffer())
