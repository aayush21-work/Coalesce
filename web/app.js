"use strict";

const strategies = {
  write_through: ["Write-through", "One UPDATE per like. Saved on return, slowest."],
  mutex_batch: ["Mutex + batch", "A locked integer, flushed on a timer."],
  atomic_batch: ["Atomic + batch", "One atomic shared by every core."],
  sharded_batch: ["Sharded + batch", "One cache line per thread, summed on flush."],
  racy: ["Racy", ""]
};
const points_max = 240;
const point_seconds = 0.25;
const reduced_motion = matchMedia("(prefers-reduced-motion: reduce)");

const by_id = id => document.getElementById(id);
const full = new Intl.NumberFormat("en-US");
const compact = new Intl.NumberFormat("en-US", { notation: "compact", maximumSignificantDigits: 3 });

function short(n) {
  return Math.abs(n) < 10000 ? full.format(Math.round(n)) : compact.format(n);
}
function rate(n) {
  return n > 0 && n < 10 ? n.toFixed(1) : short(n);
}
function time(ns) {
  if (ns === null || ns === undefined) return "-";
  if (ns < 1000) return ns + " ns";
  if (ns < 1e6) return (ns / 1e3).toPrecision(3) + " µs";
  if (ns < 1e9) return (ns / 1e6).toPrecision(3) + " ms";
  return (ns / 1e9).toPrecision(3) + " s";
}
function set_text(id, text) {
  const el = by_id(id);
  if (el.textContent !== text) el.textContent = text;
}

async function call(method, path, body) {
  const options = { method };
  if (body) {
    options.headers = { "Content-Type": "application/json" };
    options.body = JSON.stringify(body);
  }
  const res = await fetch(path, options);
  const data = await res.json();
  if (!res.ok) throw new Error(data.error || "request failed");
  return data;
}

/* counter */

let shown_count = 0;
let target_count = 0;
let counting = false;

function show_count(n) {
  target_count = n;
  if (reduced_motion.matches) {
    shown_count = n;
    set_text("count", full.format(n));
    return;
  }
  if (!counting) {
    counting = true;
    requestAnimationFrame(step_count);
  }
}
function step_count() {
  const rest = target_count - shown_count;
  shown_count = Math.abs(rest) < 1 ? target_count : shown_count + Math.sign(rest) * Math.ceil(Math.abs(rest) * 0.25);
  set_text("count", full.format(shown_count));
  counting = shown_count !== target_count;
  if (counting) requestAnimationFrame(step_count);
}

let note_timer = 0;
function note(text) {
  set_text("action_note", text);
  clearTimeout(note_timer);
  note_timer = setTimeout(() => set_text("action_note", ""), 4000);
}

async function change(path) {
  try {
    show_count((await call("POST", path)).count);
  } catch (e) {
    note("Not counted: " + e.message);
  }
}
by_id("like").onclick = () => change("/like");
by_id("dislike").onclick = () => change("/dislike");

by_id("burst").onclick = async () => {
  const button = by_id("burst");
  const total = 500;
  let sent = 0;
  let ok = 0;
  button.disabled = true;
  const started = performance.now();
  const worker = async () => {
    while (sent < total) {
      sent++;
      try {
        await call("POST", "/like");
        ok++;
      } catch (e) {
        // counted as failed below
      }
    }
  };
  // browsers open at most six connections per site, the stream uses one
  await Promise.all(Array.from({ length: 5 }, worker));
  const seconds = (performance.now() - started) / 1000;
  button.disabled = false;
  note(ok === total ? "500 likes in " + seconds.toFixed(2) + " s" : total - ok + " of 500 likes failed");
};

/* strategy switch */

for (const name of ["write_through", "mutex_batch", "atomic_batch", "sharded_batch"]) {
  const label = document.createElement("label");
  label.className = "choice";
  const input = document.createElement("input");
  input.type = "radio";
  input.name = "strategy";
  input.value = name;
  const title = document.createElement("b");
  title.textContent = strategies[name][0];
  const about = document.createElement("span");
  about.textContent = strategies[name][1];
  label.append(input, title, about);
  by_id("choices").append(label);
}
let switching = 0;
by_id("choices").onchange = async event => {
  switching++;
  try {
    await call("PUT", "/api/strategy", { strategy: event.target.value });
  } catch (e) {
    note("Strategy not changed: " + e.message);
  }
  // samples already on the way still carry the old strategy
  setTimeout(() => switching--, 600);
};
function show_strategy(name) {
  if (switching > 0) return;
  const input = document.querySelector('.choice input[value="' + name + '"]');
  if (input && !input.checked) input.checked = true;
}

/* charts */

const svg_ns = "http://www.w3.org/2000/svg";
function svg(tag, attributes) {
  const el = document.createElementNS(svg_ns, tag);
  for (const key in attributes) el.setAttribute(key, attributes[key]);
  return el;
}
function nice_max(value) {
  if (value <= 4) return 4;
  const power = 10 ** Math.floor(Math.log10(value));
  for (const step of [1, 2, 4, 6, 10]) if (value <= step * power) return step * power;
}

function make_chart(id, color, name) {
  const box = by_id(id);
  const root = svg("svg", { role: "img" });
  box.append(root);
  let values = [];
  let hover = null;

  function draw() {
    const width = box.clientWidth;
    const height = box.clientHeight;
    const left = 42, right = 8, top = 10, bottom = 22;
    const max = nice_max(Math.max(0, ...values));
    const x = i => left + (points_max - values.length + i) / (points_max - 1) * (width - left - right);
    const y = v => height - bottom - v / max * (height - top - bottom);
    root.setAttribute("viewBox", "0 0 " + width + " " + height);
    root.replaceChildren();

    for (const part of [0, 0.5, 1]) {
      const line_y = y(max * part);
      root.append(svg("line", { class: "grid_line", x1: left, x2: width - right, y1: line_y, y2: line_y }));
      const label = svg("text", { x: left - 8, y: line_y + 4, "text-anchor": "end" });
      label.textContent = short(max * part);
      root.append(label);
    }
    for (const [text, at, anchor] of [["60 s ago", left, "start"], ["now", width - right, "end"]]) {
      const label = svg("text", { x: at, y: height - 4, "text-anchor": anchor });
      label.textContent = text;
      root.append(label);
    }
    if (values.length === 0) return;

    const path = values.map((v, i) => (i ? "L" : "M") + x(i).toFixed(1) + " " + y(v).toFixed(1)).join("");
    const end = values.length - 1;
    root.append(svg("path", { class: "area", fill: color, d: path + "L" + x(end) + " " + y(0) + "L" + x(0) + " " + y(0) + "Z" }));
    root.append(svg("path", { class: "series", stroke: color, d: path }));
    root.append(svg("circle", { class: "end_dot", fill: color, r: 5, cx: x(end), cy: y(values[end]) }));
    root.setAttribute("aria-label", name + ", now " + rate(values[end]) + ", highest in the last minute " + rate(Math.max(...values)));

    if (hover !== null) {
      const bounds = box.getBoundingClientRect();
      const slot = Math.round((hover - bounds.left - left) / (width - left - right) * (points_max - 1));
      const i = Math.min(end, Math.max(0, slot - (points_max - values.length)));
      root.append(svg("line", { class: "cross", x1: x(i), x2: x(i), y1: top, y2: height - bottom }));
      root.append(svg("circle", { class: "end_dot", fill: color, r: 5, cx: x(i), cy: y(values[i]) }));
      const tip = by_id("tip");
      tip.textContent = rate(values[i]) + "/s, " + Math.round((end - i) * point_seconds) + " s ago";
      tip.hidden = false;
      const tip_x = Math.min(innerWidth - tip.offsetWidth - 8, Math.max(8, bounds.left + x(i) - tip.offsetWidth / 2));
      tip.style.left = tip_x + "px";
      tip.style.top = bounds.top + y(values[i]) - tip.offsetHeight - 12 + "px";
    }
  }
  box.onpointermove = event => { hover = event.clientX; draw(); };
  box.onpointerleave = () => { hover = null; by_id("tip").hidden = true; draw(); };
  new ResizeObserver(draw).observe(box);

  return {
    reset(list) { values = list.slice(-points_max); draw(); },
    push(v) { values.push(v); if (values.length > points_max) values.shift(); draw(); }
  };
}
const requests_chart = make_chart("requests_chart", "#e76624", "Requests per second");
const writes_chart = make_chart("writes_chart", "#338ced", "Database writes per second");

/* benchmark */

for (const name in strategies) {
  const option = document.createElement("option");
  option.value = name;
  option.textContent = strategies[name][0];
  by_id("bench_strategy").append(option);
}

by_id("bench_form").onsubmit = async event => {
  event.preventDefault();
  const form = new FormData(event.target);
  by_id("bench_error").hidden = true;
  try {
    show_bench(await call("POST", "/api/bench", {
      strategy: form.get("strategy"),
      threads: Number(form.get("threads")),
      seconds: Number(form.get("seconds"))
    }));
  } catch (e) {
    show_bench_error(e.message);
  }
};
function show_bench_error(message) {
  by_id("bench_error").textContent = "Benchmark failed: " + message;
  by_id("bench_error").hidden = false;
}

let drawn_results = "";
function show_bench(bench) {
  by_id("bench_run").disabled = bench.running;
  set_text("bench_run", bench.running ? "Running" : "Run");
  by_id("progress").hidden = !bench.running;
  if (bench.error) show_bench_error(bench.error);
  if (bench.running) {
    const now = bench.current;
    const part = Math.min(1, now.elapsed / bench.seconds);
    by_id("progress_fill").style.transform = "scaleX(" + (bench.done + part) / bench.total + ")";
    const name = strategies[now.strategy] ? strategies[now.strategy][0] : "Starting";
    set_text("progress_text", name + ", " + bench.threads + (bench.threads === 1 ? " thread, " : " threads, ") + short(now.ops_per_second) + " ops/s");
  }
  by_id("bench_empty").hidden = bench.running || bench.results.length > 0;
  by_id("bench_table").hidden = bench.results.length === 0;
  by_id("clock_note").hidden = bench.results.length === 0;

  const key = JSON.stringify(bench.results);
  if (key === drawn_results) return;
  drawn_results = key;
  draw_results(bench);
}

function draw_results(bench) {
  const results = bench.results;
  if (results.length === 0) {
    by_id("bench_rows").replaceChildren();
    return;
  }
  const rates = results.map(r => Math.log10(Math.max(1, r.ops_per_second)));
  // usually 10K to 10B, so the axis does not jump while results arrive
  const low = Math.min(4, Math.floor(Math.min(...rates)));
  const high = Math.max(10, Math.ceil(Math.max(...rates)));
  const place = v => (v - low) / (high - low) * 100 + "%";

  const ticks = [];
  for (let p = low; p <= high; p++) ticks.push(p);
  by_id("bench_axis").replaceChildren(...ticks.map(p => {
    const tick = document.createElement("i");
    tick.textContent = compact.format(10 ** p);
    tick.style.left = place(p);
    return tick;
  }));

  by_id("bench_rows").replaceChildren(...results.map((r, i) => {
    const row = document.createElement("tr");
    const head = document.createElement("th");
    head.scope = "row";
    head.textContent = strategies[r.strategy][0];
    const track_cell = document.createElement("td");
    track_cell.className = "track_cell";
    const track = document.createElement("div");
    track.className = "track";
    for (const p of ticks) {
      const line = document.createElement("i");
      line.style.left = place(p);
      track.append(line);
    }
    const dot = document.createElement("div");
    dot.className = "dot";
    dot.style.left = place(rates[i]);
    dot.title = full.format(Math.round(r.ops_per_second)) + " ops/s";
    track.append(dot);
    track_cell.append(track);
    row.append(head, track_cell);

    const cell = (text, label, kind) => {
      const td = document.createElement("td");
      td.className = "num " + kind;
      td.dataset.label = label;
      td.textContent = text;
      row.append(td);
      return td;
    };
    cell(short(r.ops_per_second), "Ops/s", "ops");
    cell(time(r.p50_ns), "p50", "rest");
    cell(time(r.p99_ns), "p99", "rest");
    cell(time(r.max_ns), "Max", "rest");
    cell(short(r.db_writes), "DB writes", "rest");
    const lost = cell(r.lost === 0 ? "0" : "⚠ " + short(r.lost), "Lost", "rest");
    if (r.lost !== 0) {
      lost.classList.add("lost");
      lost.title = full.format(r.lost) + " of " + full.format(r.ops) + " updates never reached the database";
    }
    return row;
  }));
  set_text("clock_note", results[0].threads + (results[0].threads === 1 ? " thread. " : " threads. ") +
    "Latencies include about " + bench.clock_ns + " ns for reading the clock. Write-through times every call, the others one in 64.");
}

/* live stream */

let unsaved_peak = 1;
function show_state(s) {
  show_count(s.count);
  show_strategy(s.strategy);
  set_text("unsaved", full.format(s.unsaved));
  unsaved_peak = Math.max(Math.abs(s.unsaved), unsaved_peak * 0.985, 1);
  by_id("meter_fill").style.transform = "scaleX(" + Math.abs(s.unsaved) / unsaved_peak + ")";
  const risk = s.unsaved === 0 ? "Everything is saved. " : "In memory until the next flush. A crash right now would lose these. ";
  set_text("saved_text", risk + full.format(s.durable) + " in SQLite." +
    (s.flush_errors > 0 ? " " + s.flush_errors + " flushes failed and were retried." : ""));
  set_text("ratio", s.writes_per_second > 0 ? short(s.requests_per_second / s.writes_per_second) : "-");
  set_text("p50", time(s.p50_ns));
  set_text("p99", time(s.p99_ns));
  set_text("requests_now", rate(s.requests_per_second));
  set_text("writes_now", rate(s.writes_per_second));
  requests_chart.push(s.requests_per_second);
  writes_chart.push(s.writes_per_second);
  show_bench(s.bench);
  set_text("link_text", "Live, " + s.viewers + (s.viewers === 1 ? " viewer" : " viewers"));
}

function connect() {
  const source = new EventSource("/api/stream");
  source.onopen = () => { by_id("link").className = "link live"; };
  source.addEventListener("history", event => {
    // the newest point comes again as the first sample
    const list = JSON.parse(event.data).slice(0, -1);
    requests_chart.reset(list.map(p => p.requests_per_second));
    writes_chart.reset(list.map(p => p.writes_per_second));
  });
  source.onmessage = event => show_state(JSON.parse(event.data));
  source.onerror = () => {
    by_id("link").className = "link down";
    set_text("link_text", "Disconnected, retrying");
    if (source.readyState === EventSource.CLOSED) setTimeout(connect, 2000);
  };
}
connect();
