import { spawn } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import path from 'node:path'
import fs from 'node:fs'
import { createInterface } from 'node:readline'
import process from 'node:process'
import { WebSocketServer } from 'ws'

const host = '127.0.0.1'
const port = Number(process.env.UNIVERSE_WS_PORT ?? 19081)
const serverDir = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(serverDir, '../../..')
const bridgeExecutable = process.env.UNIVERSE_BRIDGE_BIN

const posixRepoRoot = repoRoot.replace(/\\/g, '/')
let bridgeCommand = ''
let bridgeArgs = []

if (bridgeExecutable !== undefined && bridgeExecutable.length > 0) {
  bridgeCommand = bridgeExecutable
  bridgeArgs = []
} else if (process.platform === 'win32') {
  bridgeCommand = 'podman'
  bridgeArgs = [
    'run',
    '--rm',
    '-i',
    '-v',
    `${posixRepoRoot}:/workspace`,
    '-w',
    '/workspace',
    'universecore-dev',
    './build/container/universe_ui_bridge',
  ]
} else {
  bridgeCommand = path.resolve(repoRoot, 'build/container/universe_ui_bridge')
  bridgeArgs = []
}

if (bridgeCommand.endsWith('universe_ui_bridge') && !bridgeCommand.startsWith('podman') && !fs.existsSync(bridgeCommand)) {
  console.error(`Bridge binary not found at ${bridgeCommand}`)
  process.exit(2)
}

const bridge = spawn(bridgeCommand, bridgeArgs, {
  cwd: repoRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
})

const allowedOrigins = new Set([
  'http://127.0.0.1:5173',
  'http://localhost:5173',
])

const wss = new WebSocketServer({ host, port, maxPayload: 16 * 1024 })
const sockets = new Set()
let bridgeAlive = true

function broadcast(message) {
  for (const socket of sockets) {
    if (socket.readyState === socket.OPEN) {
      socket.send(message)
    }
  }
}

const outputReader = createInterface({ input: bridge.stdout })
outputReader.on('line', (line) => {
  if (line.trim().length > 0) {
    broadcast(line)
  }
})

const errorReader = createInterface({ input: bridge.stderr })
errorReader.on('line', (line) => {
  if (line.trim().length > 0) {
    broadcast(JSON.stringify({ protocolVersion: 1, type: 'error', payload: { code: 'bridge_stderr', message: line } }))
  }
})

wss.on('connection', (socket, request) => {
  const originHeader = request.headers.origin
  console.error(`[debug] connection from origin=${JSON.stringify(originHeader)}`)
  if (typeof originHeader === 'string' && !allowedOrigins.has(originHeader)) {
    console.error(`[debug] REJECTING connection: origin ${JSON.stringify(originHeader)} not in allowed set`)
    socket.close(1008, 'Origin not allowed')
    return
  }

  sockets.add(socket)
  socket.on('message', (buffer) => {
    if (!bridgeAlive) {
      socket.close(1011, 'Backend unavailable')
      return
    }
    bridge.stdin.write(String(buffer).trim() + '\n')
  })
  socket.on('close', (code, reason) => {
    console.error(`[debug] socket closed code=${code} reason=${JSON.stringify(reason.toString())}`)
    sockets.delete(socket)
  })
  socket.on('error', (err) => {
    console.error(`[debug] socket error: ${err.message}`)
  })
})

bridge.on('exit', (code) => {
  bridgeAlive = false
  broadcast(JSON.stringify({ protocolVersion: 1, type: 'error', payload: { code: 'bridge_exit', message: `Bridge exited with code ${code}` } }))
  broadcast(JSON.stringify({ protocolVersion: 1, type: 'simulation_status', payload: { paused: true, speed: 0 } }))
  for (const socket of sockets) {
    socket.close(1011, 'Backend exited')
  }
  wss.close(() => process.exit(code ?? 1))
})

console.log(`UniverseCore WS bridge listening on ws://${host}:${port}`)
console.log(`Bridge command: ${bridgeCommand} ${bridgeArgs.join(' ')}`)
