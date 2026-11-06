// Micro-benchmarks for the TypeScript/JavaScript tier: what the web client and
// the WebSocket bridge do with every message the C++ simulation emits.
//
//   UC_PERF_FIXTURE=<ndjson> node bench/protocol.bench.mjs            (human table)
//   UC_PERF_FIXTURE=<ndjson> node bench/protocol.bench.mjs --json > out.json
//
// The fixture is real traffic from universe_ui_bridge (the pipeline captures it
// with scripts/perf/run-perf-pipeline.ps1, or see capture instructions in
// docs/perf/README.md), so message sizes and shapes are the real ones.
//
// What is measured:
//   parse.<type>     JSON.parse of one message of each type (client receive cost)
//   applyDelta       the same Map upserts main.ts applyWorldDelta performs
//   bridge.relay     the per-message work ws-bridge.mjs does: trim + broadcast fan-out
//                    to N sockets (string handling only, no network)
//   encode.command   JSON.stringify of a client command
//
// main.ts is DOM/PlayCanvas-bound and cannot be imported under plain node, so
// applyDelta mirrors its Map logic line for line. Update both together.

import fs from 'node:fs'
import { bench, group, run, summary } from 'mitata'

const log = process.argv.includes('--json') ? console.error : console.log

const fixturePath = process.env.UC_PERF_FIXTURE
if (!fixturePath || !fs.existsSync(fixturePath)) {
  console.error('Set UC_PERF_FIXTURE to an NDJSON capture of universe_ui_bridge output.')
  process.exit(2)
}

const lines = fs.readFileSync(fixturePath, 'utf8').split('\n').filter((l) => l.trim().startsWith('{'))
const byType = new Map()
for (const line of lines) {
  const m = /"type":"([a-z_]+)"/.exec(line)
  if (!m) continue
  if (!byType.has(m[1])) byType.set(m[1], [])
  byType.get(m[1]).push(line)
}

function biggest(type) {
  const all = byType.get(type) ?? []
  return all.reduce((best, cur) => (cur.length > (best?.length ?? -1) ? cur : best), undefined)
}

const sample = {}
for (const type of ['initial_snapshot', 'world_delta', 'simulation_status', 'world_event']) {
  sample[type] = biggest(type)
}

log(`fixture: ${lines.length} messages`)
for (const [type, all] of byType) {
  const bytes = all.reduce((n, l) => n + l.length, 0)
  log(`  ${type.padEnd(20)} count=${String(all.length).padStart(6)}  bytes=${bytes}  largest=${biggest(type).length}`)
}

const agentsById = new Map()
const settlementsById = new Map()
const buildingsById = new Map()

function applyWorldDelta(payload) {
  for (const a of payload.addedAgents ?? []) agentsById.set(a.id, a)
  for (const a of payload.updatedAgents ?? []) agentsById.set(a.id, a)
  for (const id of payload.removedAgentIds ?? []) agentsById.delete(id)
  for (const s of payload.addedSettlements ?? []) settlementsById.set(s.id, s)
  for (const s of payload.updatedSettlements ?? []) settlementsById.set(s.id, s)
  for (const id of payload.removedSettlementIds ?? []) settlementsById.delete(id)
  for (const b of payload.addedBuildings ?? []) buildingsById.set(b.id, b)
  for (const b of payload.updatedBuildings ?? []) buildingsById.set(b.id, b)
  for (const id of payload.removedBuildingIds ?? []) buildingsById.delete(id)
}

const FANOUT = 8
const sockets = Array.from({ length: FANOUT }, () => ({ sent: 0, bytes: 0 }))

group('parse', () => {
  summary(() => {
    for (const [type, line] of Object.entries(sample)) {
      if (!line) continue
      bench(`parse.${type} (${line.length} B)`, () => JSON.parse(line))
    }
  })
})

group('client', () => {
  if (sample.world_delta) {
    bench('parse+applyDelta (largest world_delta)', () => applyWorldDelta(JSON.parse(sample.world_delta).payload))
  }
  bench('encode.command', () => JSON.stringify({ type: 'set_speed', speed: 4 }))
})

group('bridge', () => {
  if (sample.world_delta) {
    bench(`bridge.relay x${FANOUT} (largest world_delta)`, () => {
      const text = String(sample.world_delta).trim()
      for (const socket of sockets) {
        socket.sent += 1
        socket.bytes += text.length
      }
    })
  }
})

await run({ format: process.argv.includes('--json') ? 'json' : 'mitata' })
