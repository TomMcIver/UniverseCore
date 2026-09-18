import * as pc from 'playcanvas'
import type { AgentSnapshot, BuildingSnapshot, SettlementSnapshot } from './protocol'

export interface PickResult {
  type: 'agent' | 'building'
  id: number
}

type PickPreference = 'any' | 'agent' | 'building'

type PrimitiveType = 'box' | 'sphere' | 'capsule' | 'cylinder' | 'cone' | 'plane'

interface AgentVisual {
  root: pc.Entity
  body: pc.Entity
  head: pc.Entity
  item: pc.Entity
}

interface BuildingVisual {
  root: pc.Entity
  foundation: pc.Entity
  shell: pc.Entity
  roof: pc.Entity
}

interface SettlementVisual {
  root: pc.Entity
  disc: pc.Entity
  beacon: pc.Entity
}

export class WorldRenderer {
  private readonly app: pc.AppBase
  private readonly canvas: HTMLCanvasElement
  private readonly camera: pc.Entity
  private readonly sunLight: pc.Entity
  private readonly fillLight: pc.Entity
  private readonly ground: pc.Entity
  private readonly terrainRim: pc.Entity

  private readonly agents = new Map<number, AgentVisual>()
  private readonly settlements = new Map<number, SettlementVisual>()
  private readonly buildings = new Map<number, BuildingVisual>()

  private readonly agentTargets = new Map<number, pc.Vec3>()
  private readonly buildingTargets = new Map<number, pc.Vec3>()
  private readonly buildingTargetHeight = new Map<number, number>()

  private readonly agentAliveById = new Map<number, boolean>()
  private readonly agentOccupationById = new Map<number, string>()
  private readonly buildingStateById = new Map<number, string>()
  private readonly buildingTypeById = new Map<number, string>()

  private selected: PickResult | null = null

  private orbitYaw = 45
  private orbitPitch = 38
  private orbitDistance = 120
  private readonly orbitTarget = new pc.Vec3(0, 0, 0)
  private rotating = false
  private panning = false
  private lastPointerX = 0
  private lastPointerY = 0
  private readonly keyState = new Set<string>()

  private frames = 0
  private fps = 0
  private fpsWindowStart = performance.now()

  private simulationPaused = true
  private simulationSpeed = 1
  private animationTime = 0

  private readonly selectedMaterial: pc.StandardMaterial
  private readonly agentAliveMaterial: pc.StandardMaterial
  private readonly agentDeadMaterial: pc.StandardMaterial
  private readonly skinMaterial: pc.StandardMaterial
  private readonly deadSkinMaterial: pc.StandardMaterial
  private readonly settlementMaterial: pc.StandardMaterial
  private readonly buildingPlannedMaterial: pc.StandardMaterial
  private readonly buildingBuildingMaterial: pc.StandardMaterial
  private readonly buildingCompleteMaterial: pc.StandardMaterial
  private readonly roofMaterial: pc.StandardMaterial
  private readonly stoneMaterial: pc.StandardMaterial
  private readonly groundMaterial: pc.StandardMaterial
  private readonly rimMaterial: pc.StandardMaterial
  private readonly occupationMaterialByName = new Map<string, pc.StandardMaterial>()

  public constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas
    this.app = new pc.Application(canvas, {
      keyboard: new pc.Keyboard(window),
      mouse: new pc.Mouse(canvas),
      touch: new pc.TouchDevice(canvas),
    })

    this.app.start()
    this.app.setCanvasFillMode(pc.FILLMODE_FILL_WINDOW)
    this.app.setCanvasResolution(pc.RESOLUTION_AUTO)

    this.selectedMaterial = this.makeMaterial(new pc.Color(1, 0.82, 0.26), 0.15, 0.2)
    this.agentAliveMaterial = this.makeMaterial(new pc.Color(0.6, 0.68, 0.8), 0.03, 0.12)
    this.agentDeadMaterial = this.makeMaterial(new pc.Color(0.3, 0.3, 0.32), 0.02, 0.04)
    this.skinMaterial = this.makeMaterial(new pc.Color(0.79, 0.66, 0.55), 0.02, 0.07)
    this.deadSkinMaterial = this.makeMaterial(new pc.Color(0.42, 0.42, 0.44), 0.01, 0.04)
    this.settlementMaterial = this.makeMaterial(new pc.Color(0.27, 0.45, 0.66), 0.03, 0.14)
    this.buildingPlannedMaterial = this.makeMaterial(new pc.Color(0.34, 0.36, 0.42), 0.02, 0.08)
    this.buildingBuildingMaterial = this.makeMaterial(new pc.Color(0.7, 0.56, 0.2), 0.04, 0.14)
    this.buildingCompleteMaterial = this.makeMaterial(new pc.Color(0.58, 0.5, 0.4), 0.05, 0.16)
    this.roofMaterial = this.makeMaterial(new pc.Color(0.44, 0.27, 0.19), 0.02, 0.11)
    this.stoneMaterial = this.makeMaterial(new pc.Color(0.5, 0.52, 0.54), 0.02, 0.07)
    this.groundMaterial = this.makeMaterial(new pc.Color(0.11, 0.14, 0.18), 0.01, 0.03)
    this.rimMaterial = this.makeMaterial(new pc.Color(0.21, 0.17, 0.13), 0.01, 0.03)

    this.occupationMaterialByName.set('SubsistenceCultivator', this.makeMaterial(new pc.Color(0.47, 0.63, 0.35), 0.02, 0.08))
    this.occupationMaterialByName.set('WildFoodGatherer', this.makeMaterial(new pc.Color(0.33, 0.58, 0.42), 0.02, 0.08))
    this.occupationMaterialByName.set('WoodGatherer', this.makeMaterial(new pc.Color(0.58, 0.4, 0.26), 0.02, 0.1))
    this.occupationMaterialByName.set('StoneExtractor', this.makeMaterial(new pc.Color(0.5, 0.54, 0.61), 0.02, 0.1))
    this.occupationMaterialByName.set('CraftWorker', this.makeMaterial(new pc.Color(0.64, 0.49, 0.23), 0.02, 0.1))
    this.occupationMaterialByName.set('None', this.makeMaterial(new pc.Color(0.5, 0.5, 0.54), 0.02, 0.06))

    this.camera = new pc.Entity('Camera')
    this.camera.addComponent('camera', {
      clearColor: new pc.Color(0.04, 0.05, 0.07),
      farClip: 2000,
      nearClip: 0.1,
      fov: 46,
    })
    this.updateCameraPose()

    this.app.scene.ambientLight = new pc.Color(0.26, 0.29, 0.35)

    this.sunLight = new pc.Entity('SunLight')
    this.sunLight.addComponent('light', {
      type: 'directional',
      castShadows: true,
      intensity: 1.35,
      shadowDistance: 340,
      shadowResolution: 2048,
    })
    this.sunLight.setEulerAngles(54, 26, 0)

    this.fillLight = new pc.Entity('FillLight')
    this.fillLight.addComponent('light', {
      type: 'directional',
      castShadows: false,
      intensity: 0.38,
      color: new pc.Color(0.67, 0.75, 0.9),
    })
    this.fillLight.setEulerAngles(24, -120, 0)

    this.ground = this.createRenderPart('Ground', 'plane', this.groundMaterial)
    this.ground.setLocalScale(720, 1, 720)

    this.terrainRim = this.createRenderPart('TerrainRim', 'cylinder', this.rimMaterial)
    this.terrainRim.setLocalScale(180, 0.45, 180)
    this.terrainRim.setPosition(0, -0.28, 0)

    this.app.root.addChild(this.camera)
    this.app.root.addChild(this.sunLight)
    this.app.root.addChild(this.fillLight)
    this.app.root.addChild(this.ground)
    this.app.root.addChild(this.terrainRim)

    this.bindCameraInput()
    this.app.on('update', (dt: number) => {
      this.updateInterpolation(dt)
      this.updateMovement(dt)
      this.updateFps()
    })
  }

  public setSettlements(settlements: SettlementSnapshot[]): void {
    const seen = new Set<number>()
    for (const settlement of settlements) {
      seen.add(settlement.id)
      let visual = this.settlements.get(settlement.id)
      if (visual === undefined) {
        visual = this.createSettlementVisual(settlement.id)
        this.app.root.addChild(visual.root)
        this.settlements.set(settlement.id, visual)
      }
      visual.root.setPosition(settlement.position.x, 0.1, settlement.position.z)
    }

    for (const [id, visual] of this.settlements.entries()) {
      if (!seen.has(id)) {
        visual.root.destroy()
        this.settlements.delete(id)
      }
    }
  }

  public setAgents(agents: AgentSnapshot[]): void {
    const seen = new Set<number>()
    for (const agent of agents) {
      seen.add(agent.id)
      let visual = this.agents.get(agent.id)
      if (visual === undefined) {
        visual = this.createAgentVisual(agent.id, agent.occupation)
        this.app.root.addChild(visual.root)
        this.agents.set(agent.id, visual)
        visual.root.setPosition(agent.position.x, agent.position.y, agent.position.z)
      }

      this.agentAliveById.set(agent.id, agent.alive)
      this.agentOccupationById.set(agent.id, agent.occupation)

      const target = this.agentTargets.get(agent.id)
      if (target === undefined) {
        this.agentTargets.set(agent.id, new pc.Vec3(agent.position.x, agent.position.y, agent.position.z))
      } else {
        target.set(agent.position.x, agent.position.y, agent.position.z)
      }

      const currentTarget = this.agentTargets.get(agent.id)
      if (currentTarget !== undefined) {
        const current = visual.root.getPosition()
        const dx = currentTarget.x - current.x
        const dz = currentTarget.z - current.z
        if ((dx * dx) + (dz * dz) > 0.0008) {
          visual.root.setEulerAngles(0, (Math.atan2(dx, dz) * 180) / Math.PI, 0)
        }
      }

      this.applyAgentMaterial(agent.id, this.selected?.type === 'agent' && this.selected.id === agent.id)
    }

    for (const [id, visual] of this.agents.entries()) {
      if (!seen.has(id)) {
        visual.root.destroy()
        this.agents.delete(id)
        this.agentTargets.delete(id)
        this.agentAliveById.delete(id)
        this.agentOccupationById.delete(id)
      }
    }
  }

  public setBuildings(buildings: BuildingSnapshot[]): void {
    const seen = new Set<number>()
    for (const building of buildings) {
      seen.add(building.id)

      let visual = this.buildings.get(building.id)
      const previousType = this.buildingTypeById.get(building.id)
      if (visual !== undefined && previousType !== undefined && previousType !== building.type) {
        visual.root.destroy()
        this.buildings.delete(building.id)
        visual = undefined
      }

      if (visual === undefined) {
        visual = this.createBuildingVisual(building.id, building.type)
        this.app.root.addChild(visual.root)
        this.buildings.set(building.id, visual)
      }

      this.buildingTypeById.set(building.id, building.type)
      this.buildingStateById.set(building.id, building.state)

      const progress = Math.max(0, Math.min(1, building.progress))
      const targetHeight = 0.4 + progress * 2.4
      this.buildingTargetHeight.set(building.id, targetHeight)

      const targetPos = this.buildingTargets.get(building.id)
      if (targetPos === undefined) {
        this.buildingTargets.set(building.id, new pc.Vec3(building.position.x, targetHeight * 0.5, building.position.z))
        visual.root.setPosition(building.position.x, targetHeight * 0.5, building.position.z)
      } else {
        targetPos.set(building.position.x, targetHeight * 0.5, building.position.z)
      }

      const footprint = this.resolveBuildingFootprint(building.type)
      visual.root.setLocalScale(footprint, targetHeight, footprint)

      this.applyBuildingMaterial(building.id, this.selected?.type === 'building' && this.selected.id === building.id)
    }

    for (const [id, visual] of this.buildings.entries()) {
      if (!seen.has(id)) {
        visual.root.destroy()
        this.buildings.delete(id)
        this.buildingTypeById.delete(id)
        this.buildingStateById.delete(id)
        this.buildingTargets.delete(id)
        this.buildingTargetHeight.delete(id)
      }
    }
  }

  public pick(screenX: number, screenY: number, preference: PickPreference = 'any'): PickResult | null {
    let best: PickResult | null = null
    let bestDistance = 24
    const camera = this.camera.camera
    if (camera === undefined) {
      return null
    }

    if (preference !== 'building') {
      for (const [id, visual] of this.agents.entries()) {
        const screen = camera.worldToScreen(visual.root.getPosition())
        if (screen.z <= 0) {
          continue
        }
        const dx = screen.x - screenX
        const dy = screen.y - screenY
        const dist = Math.sqrt(dx * dx + dy * dy)
        if (dist < bestDistance) {
          bestDistance = dist
          best = { type: 'agent', id }
        }
      }
    }

    if (preference !== 'agent') {
      for (const [id, visual] of this.buildings.entries()) {
        const screen = camera.worldToScreen(visual.root.getPosition())
        if (screen.z <= 0) {
          continue
        }
        const dx = screen.x - screenX
        const dy = screen.y - screenY
        const dist = Math.sqrt(dx * dx + dy * dy) - (preference === 'building' ? 10 : 5)
        if (dist < bestDistance) {
          bestDistance = dist
          best = { type: 'building', id }
        }
      }
    }

    return best
  }

  public select(target: PickResult | null): void {
    this.selected = target
    this.applySelectionMaterial()
  }

  public focus(target: PickResult): void {
    const visual = target.type === 'agent' ? this.agents.get(target.id) : this.buildings.get(target.id)
    if (visual === undefined) {
      return
    }
    const position = visual.root.getPosition()
    this.orbitTarget.set(position.x, 0, position.z)
    this.orbitDistance = 26
    this.orbitPitch = 35
    this.updateCameraPose()
  }

  public metrics(): { fps: number; drawCalls: number; triangles: number; renderedEntities: number } {
    const drawCalls = (this.app.stats as { drawCalls?: { total?: number } }).drawCalls?.total ?? 0
    const triangles = (this.app.stats as { frame?: { triangles?: number } }).frame?.triangles ?? 0
    return {
      fps: this.fps,
      drawCalls,
      triangles,
      renderedEntities: this.agents.size + this.buildings.size + this.settlements.size,
    }
  }

  public setSimulationState(paused: boolean, speed: number): void {
    this.simulationPaused = paused
    this.simulationSpeed = Math.max(1, speed)
  }

  private updateInterpolation(dt: number): void {
    if (!this.simulationPaused) {
      this.animationTime += dt * this.simulationSpeed
    }

    const blend = Math.min(1, dt * 8)

    for (const [id, visual] of this.agents.entries()) {
      const target = this.agentTargets.get(id)
      if (target === undefined) {
        continue
      }

      const phase = (id % 997) * 0.013
      const wander = this.simulationPaused ? 0 : 0.55
      const animatedX = target.x + Math.cos(this.animationTime * 0.9 + phase) * wander
      const animatedZ = target.z + Math.sin(this.animationTime * 1.1 + phase) * wander

      const current = visual.root.getPosition()
      visual.root.setPosition(
        current.x + (animatedX - current.x) * blend,
        current.y + (target.y - current.y) * blend,
        current.z + (animatedZ - current.z) * blend,
      )

      const bob = this.simulationPaused ? 0 : 0.08 * Math.sin(this.animationTime * 4 + (id % 37))
      visual.head.setLocalPosition(0, 1.08 + bob, 0)
    }

    for (const [id, visual] of this.buildings.entries()) {
      const target = this.buildingTargets.get(id)
      if (target === undefined) {
        continue
      }

      const current = visual.root.getPosition()
      visual.root.setPosition(
        current.x + (target.x - current.x) * blend,
        current.y + (target.y - current.y) * blend,
        current.z + (target.z - current.z) * blend,
      )

      const targetHeight = this.buildingTargetHeight.get(id)
      if (targetHeight !== undefined) {
        const type = this.buildingTypeById.get(id) ?? 'Dwelling'
        const footprint = this.resolveBuildingFootprint(type)
        const scale = visual.root.getLocalScale()
        visual.root.setLocalScale(footprint, scale.y + (targetHeight - scale.y) * blend, footprint)
      }
    }
  }

  private bindCameraInput(): void {
    this.canvas.addEventListener('contextmenu', (event) => event.preventDefault())

    this.canvas.addEventListener('mousedown', (event) => {
      if (event.button === 1) {
        this.panning = true
      }
      if (event.button === 2) {
        this.rotating = true
      }
      this.lastPointerX = event.clientX
      this.lastPointerY = event.clientY
    })

    window.addEventListener('mouseup', () => {
      this.panning = false
      this.rotating = false
    })

    window.addEventListener('mousemove', (event) => {
      const dx = event.clientX - this.lastPointerX
      const dy = event.clientY - this.lastPointerY
      this.lastPointerX = event.clientX
      this.lastPointerY = event.clientY

      if (this.rotating) {
        this.orbitYaw -= dx * 0.22
        this.orbitPitch = Math.max(12, Math.min(85, this.orbitPitch - dy * 0.18))
        this.updateCameraPose()
      }

      if (this.panning) {
        const right = this.camera.right.clone().mulScalar(-dx * 0.13)
        const forward = this.camera.forward.clone().mulScalar(dy * 0.13)
        forward.y = 0
        this.orbitTarget.add(right).add(forward)
        this.updateCameraPose()
      }
    })

    this.canvas.addEventListener('wheel', (event) => {
      this.orbitDistance = Math.max(8, Math.min(420, this.orbitDistance + event.deltaY * 0.08))
      this.updateCameraPose()
    })

    window.addEventListener('keydown', (event) => this.keyState.add(event.key.toLowerCase()))
    window.addEventListener('keyup', (event) => this.keyState.delete(event.key.toLowerCase()))
  }

  private updateMovement(dt: number): void {
    const baseSpeed = Math.max(8, this.orbitDistance * 0.25)
    const move = baseSpeed * dt

    const forward = this.camera.forward.clone()
    forward.y = 0
    forward.normalize()
    const right = this.camera.right.clone()
    right.y = 0
    right.normalize()

    let moved = false
    if (this.keyState.has('w')) {
      this.orbitTarget.add(forward.clone().mulScalar(move))
      moved = true
    }
    if (this.keyState.has('s')) {
      this.orbitTarget.add(forward.clone().mulScalar(-move))
      moved = true
    }
    if (this.keyState.has('a')) {
      this.orbitTarget.add(right.clone().mulScalar(-move))
      moved = true
    }
    if (this.keyState.has('d')) {
      this.orbitTarget.add(right.clone().mulScalar(move))
      moved = true
    }

    if (moved) {
      this.updateCameraPose()
    }
  }

  private updateCameraPose(): void {
    const yaw = (this.orbitYaw * Math.PI) / 180
    const pitch = (this.orbitPitch * Math.PI) / 180
    const x = this.orbitTarget.x + this.orbitDistance * Math.cos(pitch) * Math.sin(yaw)
    const y = this.orbitTarget.y + this.orbitDistance * Math.sin(pitch)
    const z = this.orbitTarget.z + this.orbitDistance * Math.cos(pitch) * Math.cos(yaw)
    this.camera.setPosition(x, y, z)
    this.camera.lookAt(this.orbitTarget)
  }

  private updateFps(): void {
    this.frames += 1
    const now = performance.now()
    const elapsed = now - this.fpsWindowStart
    if (elapsed >= 500) {
      this.fps = (this.frames * 1000) / elapsed
      this.frames = 0
      this.fpsWindowStart = now
    }
  }

  private resolveBuildingFootprint(type: string): number {
    if (type === 'FoodStore') {
      return 1.55
    }
    if (type === 'Workshop') {
      return 1.35
    }
    return 1.1
  }

  private resolveBuildingMaterial(state: string): pc.StandardMaterial {
    if (state === 'Complete') {
      return this.buildingCompleteMaterial
    }
    if (state === 'ActiveConstruction') {
      return this.buildingBuildingMaterial
    }
    return this.buildingPlannedMaterial
  }

  private resolveOccupationMaterial(occupation: string): pc.StandardMaterial {
    return this.occupationMaterialByName.get(occupation) ?? this.occupationMaterialByName.get('None') ?? this.agentAliveMaterial
  }

  private resolveOccupationItemPrimitive(occupation: string): PrimitiveType {
    if (occupation === 'CraftWorker' || occupation === 'StoneExtractor') {
      return 'box'
    }
    if (occupation === 'WoodGatherer') {
      return 'cylinder'
    }
    return 'cone'
  }

  private resolveBuildingShellPrimitive(type: string): PrimitiveType {
    if (type === 'FoodStore') {
      return 'cylinder'
    }
    if (type === 'Dwelling') {
      return 'cylinder'
    }
    return 'box'
  }

  private applySelectionMaterial(): void {
    for (const id of this.agents.keys()) {
      this.applyAgentMaterial(id, this.selected?.type === 'agent' && this.selected.id === id)
    }
    for (const id of this.buildings.keys()) {
      this.applyBuildingMaterial(id, this.selected?.type === 'building' && this.selected.id === id)
    }
  }

  private applyAgentMaterial(agentId: number, selected: boolean): void {
    const visual = this.agents.get(agentId)
    if (visual === undefined) {
      return
    }

    if (selected) {
      visual.body.render!.material = this.selectedMaterial
      visual.head.render!.material = this.selectedMaterial
      visual.item.render!.material = this.selectedMaterial
      return
    }

    const alive = this.agentAliveById.get(agentId) ?? true
    const occupation = this.agentOccupationById.get(agentId) ?? 'None'
    visual.body.render!.material = alive ? this.agentAliveMaterial : this.agentDeadMaterial
    visual.head.render!.material = alive ? this.skinMaterial : this.deadSkinMaterial
    visual.item.render!.material = alive ? this.resolveOccupationMaterial(occupation) : this.agentDeadMaterial
  }

  private applyBuildingMaterial(buildingId: number, selected: boolean): void {
    const visual = this.buildings.get(buildingId)
    if (visual === undefined) {
      return
    }

    if (selected) {
      visual.foundation.render!.material = this.selectedMaterial
      visual.shell.render!.material = this.selectedMaterial
      visual.roof.render!.material = this.selectedMaterial
      return
    }

    visual.foundation.render!.material = this.stoneMaterial
    visual.shell.render!.material = this.resolveBuildingMaterial(this.buildingStateById.get(buildingId) ?? 'Planned')
    visual.roof.render!.material = this.roofMaterial
  }

  private createSettlementVisual(settlementId: number): SettlementVisual {
    const root = new pc.Entity(`settlement_${settlementId}`)

    const disc = this.createRenderPart(`settlement_${settlementId}_disc`, 'cylinder', this.settlementMaterial)
    disc.setLocalScale(2.3, 0.2, 2.3)
    disc.setLocalPosition(0, 0.1, 0)

    const beacon = this.createRenderPart(`settlement_${settlementId}_beacon`, 'cone', this.roofMaterial)
    beacon.setLocalScale(0.62, 1.05, 0.62)
    beacon.setLocalPosition(0, 0.76, 0)

    root.addChild(disc)
    root.addChild(beacon)

    return { root, disc, beacon }
  }

  private createAgentVisual(agentId: number, occupation: string): AgentVisual {
    const root = new pc.Entity(`agent_${agentId}`)

    const body = this.createRenderPart(`agent_${agentId}_body`, 'capsule', this.agentAliveMaterial)
    body.setLocalScale(0.36, 0.7, 0.3)
    body.setLocalPosition(0, 0.5, 0)

    const head = this.createRenderPart(`agent_${agentId}_head`, 'sphere', this.skinMaterial)
    head.setLocalScale(0.22, 0.22, 0.22)
    head.setLocalPosition(0, 1.08, 0)

    const item = this.createRenderPart(`agent_${agentId}_item`, this.resolveOccupationItemPrimitive(occupation), this.resolveOccupationMaterial(occupation))
    item.setLocalScale(0.14, 0.36, 0.14)
    item.setLocalPosition(0.2, 0.62, 0.13)
    item.setLocalEulerAngles(0, 0, 28)

    root.addChild(body)
    root.addChild(head)
    root.addChild(item)

    return { root, body, head, item }
  }

  private createBuildingVisual(buildingId: number, type: string): BuildingVisual {
    const root = new pc.Entity(`building_${buildingId}`)

    const foundation = this.createRenderPart(`building_${buildingId}_foundation`, 'box', this.stoneMaterial)
    foundation.setLocalScale(1.0, 0.16, 1.0)
    foundation.setLocalPosition(0, 0.08, 0)

    const shell = this.createRenderPart(`building_${buildingId}_shell`, this.resolveBuildingShellPrimitive(type), this.buildingCompleteMaterial)
    shell.setLocalScale(type === 'Workshop' ? 0.9 : 0.8, 0.62, type === 'FoodStore' ? 0.9 : 0.8)
    shell.setLocalPosition(0, 0.54, 0)

    const roofPrimitive: PrimitiveType = type === 'Workshop' ? 'box' : 'cone'
    const roof = this.createRenderPart(`building_${buildingId}_roof`, roofPrimitive, this.roofMaterial)
    roof.setLocalScale(type === 'Workshop' ? 0.9 : 0.82, 0.44, type === 'FoodStore' ? 0.92 : 0.82)
    roof.setLocalPosition(0, 1.04, 0)

    root.addChild(foundation)
    root.addChild(shell)
    root.addChild(roof)

    return { root, foundation, shell, roof }
  }

  private createRenderPart(name: string, primitive: PrimitiveType, material: pc.StandardMaterial): pc.Entity {
    const entity = new pc.Entity(name)
    entity.addComponent('render', {
      type: primitive,
      material,
      castShadows: true,
      receiveShadows: true,
    })
    return entity
  }

  private makeMaterial(color: pc.Color, emissive = 0.02, gloss = 0.08): pc.StandardMaterial {
    const material = new pc.StandardMaterial()
    material.diffuse = color
    material.emissive = color.clone().mulScalar(emissive)
    material.gloss = gloss
    material.metalness = 0.04
    material.update()
    return material
  }
}
