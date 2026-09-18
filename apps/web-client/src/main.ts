import './styles.css'

import { UniverseNetwork } from './network'
import type {
  AgentSnapshot,
  BuildingSnapshot,
  ConnectionState,
  EntityDetailsPayload,
  ServerEnvelope,
  SimulationStatusPayload,
  WorldPayload,
} from './protocol'
import { type PickResult, WorldRenderer } from './render'

const root = document.getElementById('app')
if (root === null) {
  throw new Error('app root missing')
}

root.innerHTML = `
  <div class="layout">
    <div class="topbar">
      <span id="conn">Connecting</span>
      <span id="sim">Year 0 · Tick 0</span>
      <span id="hint">Tip: Shift+Click prefers buildings</span>
      <button id="pause">Pause</button>
      <button id="resume">Resume</button>
      <button data-speed="1">1x</button>
      <button data-speed="2">2x</button>
      <button data-speed="4">4x</button>
      <button data-speed="8">8x</button>
      <button id="step">Step</button>
    </div>
    <div class="content">
      <canvas id="viewport"></canvas>
      <aside class="inspector" id="inspector">
        <h3>Inspector</h3>
        <div id="inspectorBody">Select an agent</div>
      </aside>
    </div>
    <div class="events" id="events"></div>
    <div class="debug hidden" id="debug"></div>
  </div>
`

const canvas = document.getElementById('viewport') as HTMLCanvasElement
const inspectorBody = document.getElementById('inspectorBody') as HTMLDivElement
const connEl = document.getElementById('conn') as HTMLSpanElement
const simEl = document.getElementById('sim') as HTMLSpanElement
const eventsEl = document.getElementById('events') as HTMLDivElement
const debugEl = document.getElementById('debug') as HTMLDivElement

const renderer = new WorldRenderer(canvas)
let selectedTarget: PickResult | null = null
let connectionState: ConnectionState = 'Connecting'
let tick = 0
let year = 0
let worldAgents: AgentSnapshot[] = []
let worldBuildings: BuildingSnapshot[] = []
let status: SimulationStatusPayload = { paused: true, speed: 1 }
let previousIncomingMessages = 0
let previousOutgoingMessages = 0
let previousIncomingBytes = 0
let previousOutgoingBytes = 0
let messageRateIn = 0
let messageRateOut = 0
let kbpsIn = 0
let kbpsOut = 0
let latestSnapshotSizeBytes = 0
let latestDeltaSizeBytes = 0
renderer.setSimulationState(status.paused, status.speed)

function renderInspector(details: EntityDetailsPayload): void {
  const mother = details.family.motherId > 0 ? details.family.motherId : 'Unavailable'
  const father = details.family.fatherId > 0 ? details.family.fatherId : 'Unavailable'
  inspectorBody.innerHTML = `
    <div><b>ID:</b> ${details.id}</div>
    <div><b>Age:</b> ${details.age}</div>
    <div><b>Sex:</b> ${details.sex}</div>
    <div><b>Occupation:</b> ${details.occupation}</div>
    <div><b>Alive:</b> ${details.alive}</div>
    <div><b>Household:</b> ${details.householdId}</div>
    <div><b>Settlement:</b> ${details.settlementId}</div>
    <div><b>Region:</b> ${details.regionId}</div>
    <div><b>Nutrition:</b> ${details.nutrition}</div>
    <div><b>Health:</b> ${details.health}</div>
    <div><b>Memory Summary:</b> ${details.memoryCount} items</div>
    <div><b>Knowledge Summary:</b> ${details.knowledgeCount} items</div>
    <div><b>Mother:</b> ${mother}</div>
    <div><b>Father:</b> ${father}</div>
    <div><b>Current Activity:</b> Unavailable</div>
    <div><b>Current Goal:</b> Unavailable</div>
    <div><b>Relationships:</b> Unavailable</div>
    <div><b>Personality:</b> Unavailable</div>
    <div><b>Needs/Drives:</b> Unavailable</div>
    <div><b>Wealth:</b> Unavailable</div>
    <div><b>Income:</b> Unavailable</div>
    <div><b>Schedule:</b> Unavailable</div>
    <div><b>Recent Events:</b> Unavailable</div>
  `
}

function renderBuildingInspector(building: BuildingSnapshot): void {
  const owner = building.ownerHouseholdId > 0 ? building.ownerHouseholdId : 'None'
  inspectorBody.innerHTML = `
    <div><b>ID:</b> ${building.id}</div>
    <div><b>Type:</b> ${building.type}</div>
    <div><b>State:</b> ${building.state}</div>
    <div><b>Progress:</b> ${(building.progress * 100).toFixed(0)}%</div>
    <div><b>Settlement:</b> ${building.settlementId}</div>
    <div><b>Region:</b> ${building.regionId}</div>
    <div><b>Commons Owned:</b> ${building.settlementCommonsOwned}</div>
    <div><b>Owner Household:</b> ${owner}</div>
  `
}

function findBuilding(id: number): BuildingSnapshot | undefined {
  return worldBuildings.find((candidate) => candidate.id === id)
}

function setConnectionState(next: ConnectionState): void {
  connectionState = next
  connEl.textContent = `Connection: ${next}`
}

function appendEvent(text: string): void {
  const stamp = new Date().toLocaleTimeString()
  const line = document.createElement('div')
  line.textContent = `${stamp} · ${text}`
  eventsEl.prepend(line)
  while (eventsEl.childNodes.length > 40) {
    eventsEl.removeChild(eventsEl.lastChild as ChildNode)
  }
}

const network = new UniverseNetwork(
  (message) => {
    const serialized = JSON.stringify(message)
    if (message.tick !== undefined) {
      tick = message.tick
    }

    if (message.type === 'initial_snapshot' || message.type === 'world_delta') {
      if (message.type === 'initial_snapshot') {
        latestSnapshotSizeBytes = new Blob([serialized]).size
      } else {
        latestDeltaSizeBytes = new Blob([serialized]).size
      }
      const payload = message.payload as unknown as WorldPayload
      year = payload.year
      worldAgents = payload.agents
      worldBuildings = payload.buildings
      renderer.setSettlements(payload.settlements)
      renderer.setBuildings(payload.buildings)
      renderer.setAgents(payload.agents)
      renderer.setSimulationState(status.paused, status.speed)
      simEl.textContent = `Year ${year} · Tick ${tick} · Speed ${status.speed}x · ${status.paused ? 'Paused' : 'Running'}`
      if (selectedTarget?.type === 'agent') {
        network.send({ type: 'request_entity_details', entityId: selectedTarget.id })
      }
      if (selectedTarget?.type === 'building') {
        const selected = findBuilding(selectedTarget.id)
        if (selected !== undefined) {
          renderBuildingInspector(selected)
        } else {
          selectedTarget = null
          renderer.select(null)
          inspectorBody.textContent = 'Select an agent or building'
        }
      }
      return
    }

    if (message.type === 'simulation_status') {
      status = message.payload as unknown as SimulationStatusPayload
      renderer.setSimulationState(status.paused, status.speed)
      simEl.textContent = `Year ${year} · Tick ${tick} · Speed ${status.speed}x · ${status.paused ? 'Paused' : 'Running'}`
      return
    }

    if (message.type === 'entity_details') {
      const details = message.payload as unknown as EntityDetailsPayload
      renderInspector(details)
      return
    }

    if (message.type === 'error') {
      appendEvent(`Error: ${String((message.payload as Record<string, unknown>).message ?? 'unknown')}`)
      return
    }

    if (message.type === 'world_event') {
      const payload = message.payload as Record<string, unknown>
      const eventType = String(payload.eventType ?? 'Unknown')
      const entityId = Number(payload.entityId ?? 0)
      const line = document.createElement('button')
      line.className = 'event-item'
      line.textContent = entityId > 0 ? `${eventType} · entity ${entityId}` : eventType
      line.onclick = () => {
        if (entityId > 0) {
          selectedTarget = { type: 'agent', id: entityId }
          renderer.select(selectedTarget)
          renderer.focus(selectedTarget)
          network.send({ type: 'request_entity_details', entityId })
        }
      }
      eventsEl.prepend(line)
      while (eventsEl.childNodes.length > 40) {
        eventsEl.removeChild(eventsEl.lastChild as ChildNode)
      }
      return
    }

    if (message.type === 'hello') {
      appendEvent('Handshake completed')
      return
    }
  },
  (stateCode) => {
    if (stateCode === 0) {
      setConnectionState('Connecting')
    } else if (stateCode === 1) {
      setConnectionState('Connected')
    } else if (stateCode === 2) {
      setConnectionState('Disconnected')
    } else {
      setConnectionState('Resynchronising')
    }
  },
)

network.connect('ws://127.0.0.1:19081')

canvas.addEventListener('click', (event) => {
  const rect = canvas.getBoundingClientRect()
  const x = ((event.clientX - rect.left) * canvas.width) / Math.max(rect.width, 1)
  const y = ((event.clientY - rect.top) * canvas.height) / Math.max(rect.height, 1)
  const pickMode = event.shiftKey ? 'building' : 'any'
  const picked = renderer.pick(x, y, pickMode)
  selectedTarget = picked
  renderer.select(picked)
  if (picked?.type === 'agent') {
    network.send({ type: 'request_entity_details', entityId: picked.id })
    return
  }
  if (picked?.type === 'building') {
    const selected = findBuilding(picked.id)
    if (selected !== undefined) {
      renderBuildingInspector(selected)
    }
    return
  }
  inspectorBody.textContent = 'Select an agent or building'
})

canvas.addEventListener('dblclick', () => {
  if (selectedTarget !== null) {
    renderer.focus(selectedTarget)
  }
})

;(document.getElementById('pause') as HTMLButtonElement).onclick = () => network.send({ type: 'pause' })
;(document.getElementById('resume') as HTMLButtonElement).onclick = () => network.send({ type: 'resume' })
;(document.getElementById('step') as HTMLButtonElement).onclick = () => network.send({ type: 'single_step' })

document.querySelectorAll('button[data-speed]').forEach((node) => {
  node.addEventListener('click', () => {
    const speed = Number((node as HTMLButtonElement).dataset.speed)
    network.send({ type: 'set_speed', speed })
  })
})

window.addEventListener('keydown', (event) => {
  if (event.key === 'F3') {
    debugEl.classList.toggle('hidden')
  }
})

setInterval(() => {
  const metrics = renderer.metrics()
  const net = network.stats()
  messageRateIn = net.incomingMessages - previousIncomingMessages
  messageRateOut = net.outgoingMessages - previousOutgoingMessages
  kbpsIn = (net.incomingBytes - previousIncomingBytes) / 1024
  kbpsOut = (net.outgoingBytes - previousOutgoingBytes) / 1024
  previousIncomingMessages = net.incomingMessages
  previousOutgoingMessages = net.outgoingMessages
  previousIncomingBytes = net.incomingBytes
  previousOutgoingBytes = net.outgoingBytes
  debugEl.innerHTML = `
    <div>FPS: ${metrics.fps.toFixed(1)}</div>
    <div>Frame Time: ${(1000 / Math.max(metrics.fps, 0.0001)).toFixed(2)} ms</div>
    <div>Simulation Tick: ${tick}</div>
    <div>Simulation TPS: ${status.speed * 10}</div>
    <div>Client Tick: ${Math.floor(performance.now())}</div>
    <div>Server Tick: ${tick}</div>
    <div>Tick Offset: 0</div>
    <div>WebSocket Latency: Unavailable</div>
    <div>Messages/sec: in ${messageRateIn} / out ${messageRateOut}</div>
    <div>Incoming KB/sec: ${kbpsIn.toFixed(2)}</div>
    <div>Outgoing KB/sec: ${kbpsOut.toFixed(2)}</div>
    <div>Snapshot Size: ${latestSnapshotSizeBytes > 0 ? `${latestSnapshotSizeBytes} bytes` : 'Unavailable'}</div>
    <div>Delta Size: ${latestDeltaSizeBytes > 0 ? `${latestDeltaSizeBytes} bytes` : 'Unavailable'}</div>
    <div>Total Simulated Entities: ${worldAgents.length + worldBuildings.length}</div>
    <div>Visible Entities: ${worldAgents.length + worldBuildings.length}</div>
    <div>Rendered Entities: ${metrics.renderedEntities}</div>
    <div>Draw Calls: ${metrics.drawCalls}</div>
    <div>Triangles: ${metrics.triangles}</div>
    <div>Memory Usage: ${(performance as { memory?: { usedJSHeapSize?: number } }).memory?.usedJSHeapSize ?? 'Unavailable'}</div>
    <div>Connection Status: ${connectionState}</div>
  `
}, 500)
