export type ConnectionState = 'Disconnected' | 'Connecting' | 'Connected' | 'Resynchronising'

export interface AgentSnapshot {
  id: number
  householdId: number
  settlementId: number
  regionId: number
  alive: boolean
  age: number
  sex: 'Female' | 'Male'
  occupation: string
  position: { x: number; y: number; z: number }
}

export interface SettlementSnapshot {
  id: number
  regionId: number
  active: boolean
  accessSharePoints: number
  position: { x: number; y: number; z: number }
}

export interface WorldPayload {
  year: number
  population: number
  agents: AgentSnapshot[]
  settlements: SettlementSnapshot[]
}

export interface ServerEnvelope {
  protocolVersion: number
  type:
    | 'hello'
    | 'initial_snapshot'
    | 'world_delta'
    | 'entity_details'
    | 'simulation_status'
    | 'world_event'
    | 'error'
    | 'heartbeat'
  tick?: number
  payload: Record<string, unknown>
}

export interface EntityDetailsPayload {
  entityType: 'agent'
  id: number
  alive: boolean
  age: number
  sex: string
  occupation: string
  householdId: number
  settlementId: number
  regionId: number
  nutrition: number
  health: number
  memoryCount: number
  knowledgeCount: number
  family: { motherId: number; fatherId: number }
  settlement: { active: boolean; accessSharePoints: number }
}

export interface SimulationStatusPayload {
  paused: boolean
  speed: number
}

export const PROTOCOL_VERSION = 1
