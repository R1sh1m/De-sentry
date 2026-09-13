/**
 * De-Sentry brand mark: The Sentry Aegis.
 *
 * Unites the fortified sentinel shield (sovereign data protection) with the
 * tripartite peer mesh constellation (decentralized consensus & CRDT sync)
 * and an illuminated cryptographic core.
 */

import { svg } from "./dom.js";

export interface LogoOptions {
  size?: number;
  className?: string;
  animated?: boolean;
}

/**
 * Generates an inline SVG element of the De-Sentry Aegis brand mark.
 */
export function sentryLogoSvg(options: LogoOptions = {}): SVGElement {
  const size = options.size ?? 28;
  const className = options.className ?? "sentry-logo";
  const animated = options.animated ?? false;

  const root = svg("svg", {
    class: className,
    width: size,
    height: size,
    viewBox: "0 0 100 100",
    fill: "none",
    "aria-hidden": "true",
  });

  const defs = svg("defs", {});

  // Shield Gradient
  const gradShield = svg("linearGradient", {
    id: "sentry-grad-shield",
    x1: "0%",
    y1: "0%",
    x2: "100%",
    y2: "100%",
  });
  gradShield.appendChild(svg("stop", { offset: "0%", "stop-color": "#0071e3" }));
  gradShield.appendChild(svg("stop", { offset: "100%", "stop-color": "#004085" }));
  defs.appendChild(gradShield);

  // Facet Glow Gradient
  const gradGlow = svg("linearGradient", {
    id: "sentry-grad-glow",
    x1: "50%",
    y1: "0%",
    x2: "50%",
    y2: "100%",
  });
  gradGlow.appendChild(svg("stop", { offset: "0%", "stop-color": "#2997ff", "stop-opacity": "0.6" }));
  gradGlow.appendChild(svg("stop", { offset: "100%", "stop-color": "#0071e3", "stop-opacity": "0" }));
  defs.appendChild(gradGlow);

  // Core Pulse Gradient
  const gradCore = svg("radialGradient", {
    id: "sentry-grad-core",
    cx: "50%",
    cy: "50%",
    r: "50%",
  });
  gradCore.appendChild(svg("stop", { offset: "0%", "stop-color": "#ffffff" }));
  gradCore.appendChild(svg("stop", { offset: "50%", "stop-color": "#64d2ff" }));
  gradCore.appendChild(svg("stop", { offset: "100%", "stop-color": "#0071e3", "stop-opacity": "0" }));
  defs.appendChild(gradCore);

  root.appendChild(defs);

  // Outer Shield Body
  const shield = svg("path", {
    d: "M50 8 L85 22.5 C85 55 70 78 50 92 C30 78 15 55 15 22.5 Z",
    fill: "url(#sentry-grad-shield)",
    stroke: "#2997ff",
    "stroke-width": "2",
    "stroke-linejoin": "round",
  });
  root.appendChild(shield);

  // Facet Light Highlights
  const leftFacet = svg("path", {
    d: "M50 8 L15 22.5 C15 55 30 78 50 92 L50 50 Z",
    fill: "url(#sentry-grad-glow)",
  });
  root.appendChild(leftFacet);

  // Constellation Mesh Web Lines
  const meshLines = svg("g", {
    stroke: "#ffffff",
    "stroke-width": "1.75",
    "stroke-opacity": "0.55",
    "stroke-linecap": "round",
  });
  // Triangle interconnecting satellite nodes (top 50,28, right 70,60, left 30,60)
  meshLines.appendChild(svg("line", { x1: "50", y1: "28", x2: "70", y2: "60" }));
  meshLines.appendChild(svg("line", { x1: "70", y1: "60", x2: "30", y2: "60" }));
  meshLines.appendChild(svg("line", { x1: "30", y1: "60", x2: "50", y2: "28" }));
  // Spoke lines to center (50, 49)
  meshLines.appendChild(svg("line", { x1: "50", y1: "28", x2: "50", y2: "49" }));
  meshLines.appendChild(svg("line", { x1: "70", y1: "60", x2: "50", y2: "49" }));
  meshLines.appendChild(svg("line", { x1: "30", y1: "60", x2: "50", y2: "49" }));
  root.appendChild(meshLines);

  // Satellite Nodes
  const nodes = svg("g", { fill: "#ffffff" });
  nodes.appendChild(svg("circle", { cx: "50", cy: "28", r: "4.5" }));
  nodes.appendChild(svg("circle", { cx: "70", cy: "60", r: "4.5" }));
  nodes.appendChild(svg("circle", { cx: "30", cy: "60", r: "4.5" }));
  root.appendChild(nodes);

  // Central Sentinel Core
  const coreHalo = svg("circle", {
    cx: "50",
    cy: "49",
    r: "12",
    fill: "url(#sentry-grad-core)",
    class: animated ? "sentry-core-pulse" : "",
  });
  root.appendChild(coreHalo);

  const coreSpark = svg("polygon", {
    points: "50,42 55,49 50,56 45,49",
    fill: "#ffffff",
  });
  root.appendChild(coreSpark);

  return root;
}

/**
 * Generates an SVG watermark background emblem for empty or hero states.
 */
export function sentryWatermarkSvg(size = 140): SVGElement {
  const root = svg("svg", {
    class: "sentry-watermark",
    width: size,
    height: size,
    viewBox: "0 0 100 100",
    fill: "none",
    "aria-hidden": "true",
  });

  root.appendChild(
    svg("path", {
      d: "M50 8 L85 22.5 C85 55 70 78 50 92 C30 78 15 55 15 22.5 Z",
      fill: "none",
      stroke: "currentColor",
      "stroke-width": "2",
      "stroke-dasharray": "4 3",
      "stroke-linejoin": "round",
    }),
  );

  root.appendChild(
    svg("polygon", {
      points: "50,28 70,60 30,60",
      fill: "none",
      stroke: "currentColor",
      "stroke-width": "1.5",
    }),
  );

  root.appendChild(svg("circle", { cx: "50", cy: "28", r: "4", fill: "currentColor" }));
  root.appendChild(svg("circle", { cx: "70", cy: "60", r: "4", fill: "currentColor" }));
  root.appendChild(svg("circle", { cx: "30", cy: "60", r: "4", fill: "currentColor" }));
  root.appendChild(svg("polygon", { points: "50,43 55,49 50,55 45,49", fill: "currentColor" }));

  return root;
}
