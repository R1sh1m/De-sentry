/**
 * Ambient mesh-globe backdrop: an authentic Vanta-NET-style plexus rendered on Canvas2D.
 *
 * Zero fetched dependencies (no three.js / vanta.js): an organic, moving constellation
 * of drifting plexus particles with proximity-based linking lines, subtle 3D depth,
 * interactive cursor gravity, and live peer anchors from the cryptographic mesh.
 */

export type GlobeStatus = "converged" | "lagging" | "diverged" | "offline" | "supervisor";

export interface GlobeNodePoint {
  id: string;
  status: GlobeStatus;
}

export interface GlobeLink {
  a: string;
  b: string;
  live: boolean;
  fitness: number;
}

export interface GlobeHandle {
  setData(nodes: GlobeNodePoint[], links: GlobeLink[]): void;
  setOnBattery(onBattery: boolean): void;
  destroy(): void;
}

interface Particle {
  x: number;
  y: number;
  z: number;
  vx: number;
  vy: number;
  radius: number;
  phase: number;
  anchorId?: string;
  status?: GlobeStatus;
}

interface Palette {
  pointColor: string;
  lineColor: string;
  glowColor: string;
  status: Record<GlobeStatus, string>;
}

function cssVar(name: string, fallback: string): string {
  try {
    const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return value === "" ? fallback : value;
  } catch {
    return fallback;
  }
}

function readPalette(): Palette {
  // Dark-only UI: a single ramp, no prefers-color-scheme branch.
  return {
    pointColor: cssVar("--color-primary", "#2997ff"),
    lineColor: "rgba(64, 169, 255, 0.26)",
    glowColor: "rgba(41, 151, 255, 0.12)",
    status: {
      converged: cssVar("--color-status-converged", "#30d158"),
      lagging: cssVar("--color-status-lagging", "#ffd60a"),
      diverged: cssVar("--color-status-offline", "#ff453a"),
      offline: cssVar("--color-status-offline", "#ff453a"),
      supervisor: cssVar("--color-status-supervisor", "#bf5af2"),
    },
  };
}

interface Anchor {
  id: string;
  status: GlobeStatus;
  /** Base position (centered constellation, layout space = CSS px). */
  bx: number;
  by: number;
  x: number;
  y: number;
  phase: number;
}

export function createMeshGlobe(canvas: HTMLCanvasElement): GlobeHandle {
  const ctx = canvas.getContext("2d");
  let nodes: GlobeNodePoint[] = [];
  let links: GlobeLink[] = [];
  let anchors: Anchor[] = [];
  let onBattery = false;
  let destroyed = false;
  let raf = 0;
  let visible = true;

  let mouseX = -1000;
  let mouseY = -1000;

  const handleMouseMove = (e: MouseEvent) => {
    mouseX = e.clientX;
    mouseY = e.clientY;
  };
  const handleMouseLeave = () => {
    mouseX = -1000;
    mouseY = -1000;
  };
  window.addEventListener("mousemove", handleMouseMove, { passive: true });
  document.addEventListener("mouseleave", handleMouseLeave);

  const reduceQuery =
    typeof window.matchMedia === "function"
      ? window.matchMedia("(prefers-reduced-motion: reduce)")
      : null;

  let palette = readPalette();

  const CONNECT_DIST = 172;
  const CONNECT_DIST_SQ = CONNECT_DIST * CONNECT_DIST;

  let particles: Particle[] = [];

  const initParticles = (w: number, h: number) => {
    particles = [];
    // Dense enough to read fullscreen: ~250 dots at 1360x880, clamped so
    // small windows and 4K desktops both stay smooth.
    const count = Math.max(150, Math.min(300, Math.floor((w * h) / 4800)));
    for (let i = 0; i < count; i++) {
      particles.push({
        x: Math.random() * w,
        y: Math.random() * h,
        z: 0.3 + Math.random() * 0.7,
        vx: (Math.random() - 0.5) * 0.46,
        vy: (Math.random() - 0.5) * 0.46,
        radius: 1.7 + Math.random() * 2.4,
        phase: Math.random() * Math.PI * 2,
      });
    }
  };

  /** Centered constellation for real nodes: ring around viewport center. */
  const layoutAnchors = (w: number, h: number): void => {
    const cx = w / 2;
    const cy = h / 2;
    const n = nodes.length;
    const radius = n <= 1 ? 0 : Math.max(60, Math.min(220, Math.min(w, h) * 0.16));
    anchors = nodes.map((node, i) => {
      const prev = anchors.find((a) => a.id === node.id);
      let bx = cx;
      let by = cy;
      if (n > 1) {
        const angle = (i / n) * Math.PI * 2 - Math.PI / 2;
        bx = cx + Math.cos(angle) * radius;
        by = cy + Math.sin(angle) * radius * 0.72;
      }
      return {
        id: node.id,
        status: node.status,
        bx,
        by,
        x: prev?.x ?? bx,
        y: prev?.y ?? by,
        phase: prev?.phase ?? Math.random() * Math.PI * 2,
      };
    });
  };

  const resize = (): void => {
    if (destroyed) return;
    const w = Math.max(1, window.innerWidth);
    const h = Math.max(1, window.innerHeight);
    const dpr = Math.min(1.5, window.devicePixelRatio || 1);
    canvas.width = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);

    if (particles.length === 0) {
      initParticles(w, h);
    }
    layoutAnchors(w, h);
  };

  const ro: ResizeObserver | null =
    typeof ResizeObserver !== "undefined"
      ? new ResizeObserver(() => {
          resize();
          if (reduceQuery?.matches) drawStatic();
        })
      : null;
  if (canvas.parentElement !== null) ro?.observe(canvas.parentElement);
  window.addEventListener("resize", resize);
  resize();

  const io: IntersectionObserver | null =
    typeof IntersectionObserver !== "undefined"
      ? new IntersectionObserver(
          (entries) => {
            visible = entries.some((e) => e.isIntersecting);
            if (visible && !destroyed && !reduceQuery?.matches && raf === 0) {
              raf = requestAnimationFrame(tick);
            }
          },
          { threshold: 0 },
        )
      : null;
  io?.observe(canvas);

  const onVisibility = (): void => {
    if (document.hidden) {
      if (raf !== 0) cancelAnimationFrame(raf);
      raf = 0;
    } else if (visible && !destroyed && !reduceQuery?.matches && raf === 0) {
      raf = requestAnimationFrame(tick);
    }
  };
  document.addEventListener("visibilitychange", onVisibility);

  const draw = (timeS: number): void => {
    if (ctx === null) return;
    const dpr = Math.min(1.5, window.devicePixelRatio || 1);
    const w = canvas.width / dpr;
    const h = canvas.height / dpr;
    if (w < 2 || h < 2) return;

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    palette = readPalette();
    const reduceMotion = reduceQuery?.matches === true;

    // Real nodes ease toward their constellation slots; a gentle breathing
    // offset keeps them alive without ever drifting away from center.
    for (const a of anchors) {
      const breathe = reduceMotion ? 0 : 1;
      const tx = a.bx + Math.sin(timeS * 0.5 + a.phase) * 5 * breathe;
      const ty = a.by + Math.cos(timeS * 0.4 + a.phase) * 5 * breathe;
      a.x += (tx - a.x) * (reduceMotion ? 1 : 0.06);
      a.y += (ty - a.y) * (reduceMotion ? 1 : 0.06);
    }

    const speedScale = onBattery ? 0.5 : 1.0;

    // Update particles
    for (let i = 0; i < particles.length; i++) {
      const p = particles[i];
      p.x += p.vx * speedScale;
      p.y += p.vy * speedScale;

      // Wrap edges
      if (p.x < -10) p.x = w + 10;
      else if (p.x > w + 10) p.x = -10;
      if (p.y < -10) p.y = h + 10;
      else if (p.y > h + 10) p.y = -10;

      // Mouse interactive repulsion / slight attraction
      const dx = mouseX - p.x;
      const dy = mouseY - p.y;
      const distSq = dx * dx + dy * dy;
      if (distSq < 16000 && distSq > 4) {
        const dist = Math.sqrt(distSq);
        const force = (1 - dist / 126) * 0.45;
        p.x -= (dx / dist) * force;
        p.y -= (dy / dist) * force;
      }
    }

    // Draw connecting plexus lines (Vanta-NET style)
    ctx.lineWidth = 0.85;
    for (let i = 0; i < particles.length; i++) {
      const p1 = particles[i];
      for (let j = i + 1; j < particles.length; j++) {
        const p2 = particles[j];
        const dx = p1.x - p2.x;
        const dy = p1.y - p2.y;
        const distSq = dx * dx + dy * dy;
        if (distSq < CONNECT_DIST_SQ) {
          const alpha = (1 - distSq / CONNECT_DIST_SQ) * 0.44 * Math.min(p1.z, p2.z);
          ctx.strokeStyle = palette.lineColor;
          ctx.globalAlpha = alpha;
          ctx.beginPath();
          ctx.moveTo(p1.x, p1.y);
          ctx.lineTo(p2.x, p2.y);
          ctx.stroke();
        }
      }
    }

    // Draw particles (ambient dust stays decorative and dim)
    for (let i = 0; i < particles.length; i++) {
      const p = particles[i];
      const pulse = 0.8 + 0.2 * Math.sin(timeS * 1.8 + p.phase);
      const r = p.radius * p.z * pulse;

      // Glow halo
      ctx.globalAlpha = 0.16 * p.z;
      ctx.fillStyle = palette.pointColor;
      ctx.beginPath();
      ctx.arc(p.x, p.y, r * 2.6, 0, Math.PI * 2);
      ctx.fill();

      // Sharp Core
      ctx.globalAlpha = 0.6 * p.z;
      ctx.beginPath();
      ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
      ctx.fill();
    }

    // Real mesh links: the actual hierarchy. Lone nodes (no links) render as
    // independent dots; linked ones form the mesh with their fellows.
    const byId = new Map(anchors.map((a) => [a.id, a]));
    ctx.lineWidth = 1.2;
    for (const link of links) {
      const a = byId.get(link.a);
      const b = byId.get(link.b);
      if (!a || !b) continue;
      const fitness = Math.max(0.25, Math.min(1, link.fitness || 0.5));
      ctx.globalAlpha = link.live ? 0.28 + fitness * 0.5 : 0.22;
      ctx.strokeStyle = palette.lineColor;
      try {
        ctx.setLineDash(link.live ? [] : [5, 5]);
      } catch {
        // Older canvas implementations: solid fallback is fine.
      }
      ctx.beginPath();
      ctx.moveTo(a.x, a.y);
      ctx.lineTo(b.x, b.y);
      ctx.stroke();
    }
    try {
      ctx.setLineDash([]);
    } catch {
      // ignore
    }

    // Real nodes: larger, status-colored, centered behind the cards.
    for (const a of anchors) {
      const color = palette.status[a.status] ?? palette.pointColor;
      const pulse = reduceMotion ? 1 : 0.85 + 0.15 * Math.sin(timeS * 1.8 + a.phase);
      const r = 4.4 * pulse;
      ctx.globalAlpha = 0.3;
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(a.x, a.y, r * 2.6, 0, Math.PI * 2);
      ctx.fill();
      ctx.globalAlpha = 0.95;
      ctx.beginPath();
      ctx.arc(a.x, a.y, r, 0, Math.PI * 2);
      ctx.fill();
      // Crisp light core so green/converged reads at a glance.
      ctx.globalAlpha = 0.9;
      ctx.fillStyle = "#ffffff";
      ctx.beginPath();
      ctx.arc(a.x, a.y, r * 0.38, 0, Math.PI * 2);
      ctx.fill();
    }

    ctx.globalAlpha = 1.0;
  };

  const drawStatic = (): void => {
    draw(0);
  };

  const tick = (ms: number): void => {
    raf = 0;
    if (destroyed || ctx === null) return;
    if (document.hidden || !visible) return;

    draw(ms / 1000);

    if (!destroyed && reduceQuery?.matches !== true) {
      raf = requestAnimationFrame(tick);
    }
  };

  if (reduceQuery?.matches === true) {
    drawStatic();
  } else {
    raf = requestAnimationFrame(tick);
  }

  return {
    setData(nextNodes: GlobeNodePoint[], nextLinks: GlobeLink[]): void {
      const idsChanged = nextNodes.length !== nodes.length
        || nextNodes.some((n, i) => n.id !== nodes[i]?.id || n.status !== nodes[i]?.status);
      nodes = nextNodes;
      links = nextLinks.filter((l) =>
        nextNodes.some((n) => n.id === l.a) && nextNodes.some((n) => n.id === l.b),
      );
      for (const a of anchors) {
        const fresh = nextNodes.find((n) => n.id === a.id);
        if (fresh) a.status = fresh.status;
      }
      if (idsChanged) {
        try {
          const w = Math.max(1, window.innerWidth);
          const h = Math.max(1, window.innerHeight);
          layoutAnchors(w, h);
        } catch {
          // Layout happens on next resize/frame; never break render.
        }
      }
      if (reduceQuery?.matches === true) drawStatic();
    },
    setOnBattery(value: boolean): void {
      onBattery = value;
    },
    destroy(): void {
      destroyed = true;
      if (raf !== 0) cancelAnimationFrame(raf);
      raf = 0;
      ro?.disconnect();
      io?.disconnect();
      window.removeEventListener("resize", resize);
      window.removeEventListener("mousemove", handleMouseMove);
      document.removeEventListener("mouseleave", handleMouseLeave);
      document.removeEventListener("visibilitychange", onVisibility);
    },
  };
}
