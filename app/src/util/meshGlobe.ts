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

function isDarkTheme(): boolean {
  const theme = document.documentElement.getAttribute("data-theme");
  if (theme === "dark") return true;
  if (theme === "light") return false;
  return typeof window !== "undefined" && window.matchMedia?.("(prefers-color-scheme: dark)").matches;
}

function readPalette(): Palette {
  const dark = isDarkTheme();
  if (dark) {
    return {
      pointColor: cssVar("--color-primary", "#2997ff"),
      lineColor: "rgba(64, 169, 255, 0.22)",
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
  return {
    pointColor: cssVar("--color-primary", "#0066cc"),
    lineColor: "rgba(0, 102, 204, 0.18)",
    glowColor: "rgba(0, 113, 227, 0.08)",
    status: {
      converged: cssVar("--color-status-converged", "#1d7f4e"),
      lagging: cssVar("--color-status-lagging", "#8a6100"),
      diverged: cssVar("--color-status-offline", "#a1252b"),
      offline: cssVar("--color-status-offline", "#a1252b"),
      supervisor: cssVar("--color-status-supervisor", "#5a4bb8"),
    },
  };
}

export function createMeshGlobe(canvas: HTMLCanvasElement): GlobeHandle {
  const ctx = canvas.getContext("2d");
  let nodes: GlobeNodePoint[] = [];
  let onBattery = false;
  let destroyed = false;
  let raf = 0;
  let visible = true;

  let mouseX = -1000;
  let mouseY = -1000;

  const handleMouseMove = (e: MouseEvent) => {
    const rect = canvas.getBoundingClientRect();
    mouseX = e.clientX - rect.left;
    mouseY = e.clientY - rect.top;
  };
  const handleMouseLeave = () => {
    mouseX = -1000;
    mouseY = -1000;
  };
  window.addEventListener("mousemove", handleMouseMove);
  document.addEventListener("mouseleave", handleMouseLeave);

  const reduceQuery =
    typeof window.matchMedia === "function"
      ? window.matchMedia("(prefers-reduced-motion: reduce)")
      : null;

  let palette = readPalette();

  // Particle pool
  const PARTICLE_COUNT = 65;
  const CONNECT_DIST = 135;
  const CONNECT_DIST_SQ = CONNECT_DIST * CONNECT_DIST;

  let particles: Particle[] = [];

  const initParticles = (w: number, h: number) => {
    particles = [];
    for (let i = 0; i < PARTICLE_COUNT; i++) {
      particles.push({
        x: Math.random() * w,
        y: Math.random() * h,
        z: 0.2 + Math.random() * 0.8,
        vx: (Math.random() - 0.5) * 0.45,
        vy: (Math.random() - 0.5) * 0.45,
        radius: 1.5 + Math.random() * 2,
        phase: Math.random() * Math.PI * 2,
      });
    }
  };

  const resize = (): void => {
    if (destroyed) return;
    const parent = canvas.parentElement;
    const w = Math.max(1, Math.floor(parent?.clientWidth ?? window.innerWidth));
    const h = Math.max(1, Math.floor(parent?.clientHeight ?? window.innerHeight));
    const dpr = Math.min(1.5, window.devicePixelRatio || 1);
    canvas.width = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);

    if (particles.length === 0) {
      initParticles(w, h);
    }
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

  // Sync actual mesh nodes into particles
  const syncMeshNodes = () => {
    for (let i = 0; i < nodes.length && i < particles.length; i++) {
      particles[i].anchorId = nodes[i].id;
      particles[i].status = nodes[i].status;
      particles[i].radius = 3.2;
    }
  };

  const draw = (timeS: number): void => {
    if (ctx === null) return;
    const dpr = Math.min(1.5, window.devicePixelRatio || 1);
    const w = canvas.width / dpr;
    const h = canvas.height / dpr;
    if (w < 2 || h < 2) return;

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    palette = readPalette();
    syncMeshNodes();

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
          const alpha = (1 - distSq / CONNECT_DIST_SQ) * 0.32 * Math.min(p1.z, p2.z);
          ctx.strokeStyle = palette.lineColor;
          ctx.globalAlpha = alpha;
          ctx.beginPath();
          ctx.moveTo(p1.x, p1.y);
          ctx.lineTo(p2.x, p2.y);
          ctx.stroke();
        }
      }
    }

    // Draw particles
    for (let i = 0; i < particles.length; i++) {
      const p = particles[i];
      const pulse = 0.8 + 0.2 * Math.sin(timeS * 1.8 + p.phase);
      const r = p.radius * p.z * pulse;
      const color = p.status ? palette.status[p.status] : palette.pointColor;

      // Glow halo
      ctx.globalAlpha = 0.22 * p.z;
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(p.x, p.y, r * 2.6, 0, Math.PI * 2);
      ctx.fill();

      // Sharp Core
      ctx.globalAlpha = 0.85 * p.z;
      ctx.beginPath();
      ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
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
      nodes = nextNodes;
      void nextLinks;
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
