import * as pc from 'playcanvas'
import type { AgentSnapshot, SettlementSnapshot } from './protocol'

export class WorldRenderer {
  private readonly app: pc.AppBase
  private readonly canvas: HTMLCanvasElement
  private readonly camera: pc.Entity
  private readonly light: pc.Entity
  private readonly ground: pc.Entity
  private readonly agents = new Map<number, pc.Entity>()
  private readonly settlements = new Map<number, pc.Entity>()
  private selected: number | null = null
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

    this.camera = new pc.Entity('Camera')
    this.camera.addComponent('camera', {
      clearColor: new pc.Color(0.04, 0.05, 0.07),
      farClip: 2000,
      nearClip: 0.1,
      fov: 50,
    })
    this.updateCameraPose()

    this.light = new pc.Entity('DirectionalLight')
    this.light.addComponent('light', {
      type: 'directional',
      castShadows: true,
      intensity: 1.2,
      shadowDistance: 250,
    })
    this.light.setEulerAngles(50, 35, 0)

    this.ground = new pc.Entity('Ground')
    this.ground.addComponent('render', {
      type: 'plane',
      material: this.makeMaterial(new pc.Color(0.1, 0.12, 0.16)),
    })
    this.ground.setLocalScale(600, 1, 600)

    this.app.root.addChild(this.camera)
    this.app.root.addChild(this.light)
    this.app.root.addChild(this.ground)

    this.bindCameraInput()
    this.app.on('update', (dt: number) => {
      this.updateMovement(dt)
      this.updateFps()
    })
  }

  public setSettlements(settlements: SettlementSnapshot[]): void {
    const seen = new Set<number>()
    for (const settlement of settlements) {
      seen.add(settlement.id)
      const id = `settlement_${settlement.id}`
      let marker = this.settlements.get(settlement.id)
      if (marker === undefined) {
        marker = this.app.root.findByName(id) as pc.Entity | null ?? undefined
      }
      if (marker === undefined) {
        marker = new pc.Entity(id)
        marker.addComponent('render', {
          type: 'box',
          material: this.makeMaterial(new pc.Color(0.23, 0.41, 0.7)),
        })
        marker.setLocalScale(2.4, 1.2, 2.4)
        this.app.root.addChild(marker)
        this.settlements.set(settlement.id, marker)
      }
      marker.setPosition(settlement.position.x, 0.6, settlement.position.z)
    }

    for (const [id, marker] of this.settlements.entries()) {
      if (!seen.has(id)) {
        marker.destroy()
        this.settlements.delete(id)
      }
    }
  }

  public setAgents(agents: AgentSnapshot[]): void {
    const seen = new Set<number>()
    for (const agent of agents) {
      seen.add(agent.id)
      let entity = this.agents.get(agent.id)
      if (entity === undefined) {
        entity = new pc.Entity(`agent_${agent.id}`)
        entity.addComponent('render', {
          type: 'capsule',
          material: this.makeMaterial(agent.alive ? new pc.Color(0.76, 0.83, 0.94) : new pc.Color(0.3, 0.3, 0.3)),
        })
        entity.setLocalScale(0.5, 1.1, 0.5)
        this.app.root.addChild(entity)
        this.agents.set(agent.id, entity)
      }

      entity.setPosition(agent.position.x, agent.position.y, agent.position.z)
      if (this.selected === agent.id) {
        this.applySelectedMaterial(entity)
      }
    }

    for (const [id, entity] of this.agents.entries()) {
      if (!seen.has(id)) {
        entity.destroy()
        this.agents.delete(id)
      }
    }
  }

  public pick(screenX: number, screenY: number): number | null {
    let bestId: number | null = null
    let bestDistance = 24

    for (const [id, entity] of this.agents.entries()) {
      const world = entity.getPosition()
      const camera = this.camera.camera
      if (camera === undefined) {
        continue
      }
      const screen = camera.worldToScreen(world)
      const dx = screen.x - screenX
      const dy = screen.y - screenY
      const dist = Math.sqrt(dx * dx + dy * dy)
      if (dist < bestDistance) {
        bestDistance = dist
        bestId = id
      }
    }

    return bestId
  }

  public select(id: number | null): void {
    if (this.selected !== null) {
      const current = this.agents.get(this.selected)
      if (current !== undefined && current.render !== undefined) {
        current.render.material = this.makeMaterial(new pc.Color(0.76, 0.83, 0.94))
      }
    }

    this.selected = id

    if (id !== null) {
      const entity = this.agents.get(id)
      if (entity !== undefined) {
        this.applySelectedMaterial(entity)
      }
    }
  }

  public focus(id: number): void {
    const entity = this.agents.get(id)
    if (entity === undefined) {
      return
    }
    const position = entity.getPosition()
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
      renderedEntities: this.agents.size,
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

  private applySelectedMaterial(entity: pc.Entity): void {
    if (entity.render !== undefined) {
      entity.render.material = this.makeMaterial(new pc.Color(1, 0.8, 0.22))
    }
  }

  private makeMaterial(color: pc.Color): pc.StandardMaterial {
    const material = new pc.StandardMaterial()
    material.diffuse = color
    material.update()
    return material
  }
}
