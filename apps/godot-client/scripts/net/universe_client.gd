class_name UniverseClient
extends Node

## Thin WebSocket wrapper talking to UniverseCore's ws-bridge.mjs, with
## reconnect and disconnected-command handling per ticket #16.
##
## Reconnect policy: on every (re)connection, always request a fresh
## initial_snapshot and refuse to forward any world_delta until that
## snapshot has actually arrived -- never trust a delta as a valid
## continuation across a gap. This also transparently works around the
## real bug found in ticket #4: ws-bridge.mjs's C++ child emits its
## one-time hello/initial_snapshot before any client has connected, so a
## client that only waited for an unsolicited broadcast would never see it.
##
## The socket is injectable (defaults to a real WebSocketPeer) so tests can
## substitute a fake one without a live bridge -- see scripts/tests/.

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const RECONNECT_INTERVAL_SECONDS := 2.0

signal connected
signal disconnected
signal reconnecting
signal message_received(message: Dictionary)

var _socket
var _was_open := false
var _awaiting_initial_snapshot := false
var _reconnect_elapsed := 0.0

func _init(socket = null) -> void:
	if socket != null:
		_socket = socket
	else:
		# Default WebSocketPeer buffers (64 KiB) are too small for a full
		# initial_snapshot once population/buildings grow past a couple
		# hundred entities -- the server-sent snapshot gets rejected with
		# close code 1009 (message too big) before the client ever sees it.
		var real_socket := WebSocketPeer.new()
		real_socket.inbound_buffer_size = 4 * 1024 * 1024
		real_socket.outbound_buffer_size = 4 * 1024 * 1024
		_socket = real_socket

func connect_to_bridge() -> void:
	var err: int = _socket.connect_to_url(UniverseProtocol.url())
	if err != OK:
		push_error("UniverseClient: failed to start WebSocket connection (error %d)" % err)

func _process(delta: float) -> void:
	# poll() must run every frame regardless of current state -- it's what
	# actually drives WebSocketPeer's handshake state machine forward. This
	# used to be called only inside the "not closed" branch below, which
	# meant a socket sitting in STATE_CLOSED while its connection attempt
	# was still in flight never got polled, so it could never progress to
	# CONNECTING/OPEN -- the connection would just silently never complete,
	# and every RECONNECT_INTERVAL_SECONDS this code would call
	# connect_to_url() again on top of the stalled attempt. FakeSocket's
	# poll() is a no-op stub, so unit tests (which set ready_state directly)
	# never exercised this path -- only a real WebSocketPeer did.
	_socket.poll()
	var state: int = _socket.get_ready_state()

	if state == WebSocketPeer.STATE_CLOSED:
		if _was_open:
			_was_open = false
			_awaiting_initial_snapshot = false
			disconnected.emit()
		_reconnect_elapsed += delta
		if _reconnect_elapsed >= RECONNECT_INTERVAL_SECONDS:
			_reconnect_elapsed = 0.0
			reconnecting.emit()
			connect_to_bridge()
		return

	_reconnect_elapsed = 0.0

	if state != WebSocketPeer.STATE_OPEN:
		return

	if not _was_open:
		_was_open = true
		_awaiting_initial_snapshot = true
		connected.emit()
		send_command(UniverseProtocol.command_request_snapshot())

	while _socket.get_available_packet_count() > 0:
		var packet: PackedByteArray = _socket.get_packet()
		var text := packet.get_string_from_utf8()
		var message := UniverseProtocol.decode_message(text)
		if message.is_empty():
			continue

		var message_type := UniverseProtocol.message_type(message)
		if message_type == "initial_snapshot":
			_awaiting_initial_snapshot = false

		if UniverseProtocol.should_forward_message(message_type, _awaiting_initial_snapshot):
			message_received.emit(message)

## Returns true if the command was actually sent, false if it was dropped
## because the socket isn't open right now. Callers (the HUD) use this to
## show a visible failure rather than pretending the command succeeded.
func send_command(command: Dictionary) -> bool:
	if _socket.get_ready_state() != WebSocketPeer.STATE_OPEN:
		push_warning("UniverseClient: dropped command while not connected: %s" % JSON.stringify(command))
		return false
	_socket.send_text(UniverseProtocol.encode(command))
	return true

func is_connected_to_bridge() -> bool:
	return _socket.get_ready_state() == WebSocketPeer.STATE_OPEN
