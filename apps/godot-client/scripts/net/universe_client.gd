class_name UniverseClient
extends Node

## Thin WebSocketPeer wrapper talking to UniverseCore's ws-bridge.mjs.
##
## Deliberately sends request_snapshot immediately on every successful
## connection, rather than waiting for the bridge's one-time unsolicited
## hello/initial_snapshot broadcast. Ticket #4 (manual verification of the
## PlayCanvas client) found and documented a real bug: ws-bridge.mjs spawns
## the C++ bridge and it emits hello/initial_snapshot immediately, before
## any WebSocket client has connected -- so a client connecting even
## slightly after the bridge starts (i.e. every normal case) never
## receives them. Requesting a snapshot explicitly sidesteps that bug
## entirely rather than depending on winning a startup race.

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")

signal connected
signal disconnected
signal message_received(message: Dictionary)

var _socket := WebSocketPeer.new()
var _was_open := false

func connect_to_bridge() -> void:
	var err := _socket.connect_to_url(UniverseProtocol.url())
	if err != OK:
		push_error("UniverseClient: failed to start WebSocket connection (error %d)" % err)

func _process(_delta: float) -> void:
	var state_before_poll := _socket.get_ready_state()
	if state_before_poll == WebSocketPeer.STATE_CLOSED:
		if _was_open:
			_was_open = false
			disconnected.emit()
		return

	_socket.poll()
	var state := _socket.get_ready_state()

	if state == WebSocketPeer.STATE_OPEN:
		if not _was_open:
			_was_open = true
			connected.emit()
			send_command(UniverseProtocol.command_request_snapshot())
		while _socket.get_available_packet_count() > 0:
			var packet := _socket.get_packet()
			var text := packet.get_string_from_utf8()
			var message := UniverseProtocol.decode_message(text)
			if not message.is_empty():
				message_received.emit(message)
	elif state == WebSocketPeer.STATE_CLOSED and _was_open:
		_was_open = false
		disconnected.emit()

func send_command(command: Dictionary) -> void:
	if _socket.get_ready_state() != WebSocketPeer.STATE_OPEN:
		push_warning("UniverseClient: dropped command while not connected: %s" % JSON.stringify(command))
		return
	_socket.send_text(UniverseProtocol.encode(command))

func is_connected_to_bridge() -> bool:
	return _socket.get_ready_state() == WebSocketPeer.STATE_OPEN
