/* ============================================================
   Friend Recommendation Engine — frontend logic (web/app.js)
   Plain JavaScript, no framework.

   ALL DATA COMES FROM THE BACKEND. This file never scores,
   ranks or BFS-es anything itself:

     GET /api/users                     -> the people list
     GET /api/friends?user=ID           -> direct friends (1 hop)
     GET /api/recommend?user=ID&top=N   -> ranked recommendations (2 hops)
                                           + traversal counters
     GET /api/network-summary           -> {users, friendships}

   Recommendations are rendered in exactly the order the backend
   returns them (mutual friends descending). The "Show top" control
   only changes the `top` value sent to the backend; the order is
   never recomputed here.
   ============================================================ */

/* Same-origin by default (the C server also serves web/); fall back to
   the server address when the page is opened straight from disk. */
const API_BASE = (location.protocol === "file:")
  ? "http://localhost:8080/api"
  : "/api";

/* Server-side cap on `top` (MAX_TOP_N); used only to fetch the full
   2-hop candidate set for colouring the map. */
const GRAPH_TOP = 64;

/* ------------------------------------------------------------
   Data layer — the only place that knows where data comes from.
   ------------------------------------------------------------ */
const API = {
  async getUsers() {
    return (await jget("/users")).users || [];
  },
  async getFriends(userId) {
    return (await jget(`/friends?user=${encodeURIComponent(userId)}`)).friends || [];
  },
  async getRecommendations(userId, topN) {
    return await jget(`/recommend?user=${encodeURIComponent(userId)}&top=${encodeURIComponent(topN)}`);
  },
  async getSummary() {
    return await jget("/network-summary");
  }
};

async function jget(path) {
  let res;
  try {
    res = await fetch(API_BASE + path, { headers: { "Accept": "application/json" } });
  } catch (e) {
    throw new Error(`cannot reach the backend at ${API_BASE} (${e.message})`);
  }
  let body = null;
  try { body = await res.json(); } catch (e) { /* non-JSON body */ }
  if (!res.ok) {
    const msg = (body && (body.error || body.message)) || `HTTP ${res.status}`;
    throw new Error(msg);
  }
  return body || {};
}

/* ------------------------------------------------------------
   State + helpers
   ------------------------------------------------------------ */
const state = {
  users: [],
  summary: null,
  selected: null,
  friends: [],
  recs: [],
  recStats: null,
  adjacency: new Map(),   // user id -> friend ids (from /api/friends, cached)
  graphRecs: [],          // full 2-hop candidate set (top = GRAPH_TOP) for the map
  hover: null,
  view: "home",
  loadToken: 0            // guards against out-of-order async renders
};

const $ = (id) => document.getElementById(id);

const initials = (name) => String(name).split(/\s+/)
  .filter(w => w.length).map(w => w[0]).slice(0, 2).join("").toUpperCase();
const esc = (s) => String(s).replace(/[&<>"']/g, c =>
  ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

/* Rotating accent colours keep avatars lively but always on-palette. */
const AVATAR_CLASSES = ["av-blue", "av-coral", "av-amber", "av-deep"];
const avatarClass = (id) => AVATAR_CLASSES[Number(id) % AVATAR_CLASSES.length];

function userById(id) {
  return state.users.find(u => u.id === Number(id)) || null;
}
function userName(id) {
  const u = userById(id);
  return u ? u.name : `User ${id}`;
}
function userGroup(id) {
  const u = userById(id);
  return u ? u.group : "";
}

let toastTimer = null;
function toast(msg) {
  const t = $("toast");
  t.textContent = msg;
  t.classList.remove("hidden");
  t.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    t.classList.remove("show");
    t.classList.add("hidden");
  }, 2600);
}

function errorBox(container, err) {
  container.innerHTML =
    `<div class="error-box"><b>Could not load this data.</b><br>${esc(err.message)}<br>
     <span>Is the server running on port 8080?</span></div>`;
}

/* ------------------------------------------------------------
   Sidebar: community stats + person picker
   ------------------------------------------------------------ */
function renderStats() {
  const s = state.summary || {};
  $("stat-users").textContent = s.users != null ? s.users : "–";
  $("stat-edges").textContent = s.friendships != null ? s.friendships : "–";
}

function renderUserSelect() {
  $("user-select").innerHTML = state.users.slice().sort((a, b) => a.id - b.id)
    .map(u => `<option value="${u.id}"${u.id === state.selected ? " selected" : ""}>` +
              `ID ${u.id} — ${esc(u.name)}</option>`)
    .join("");
}

function renderTopbar() {
  const el = $("topbar-user");
  if (state.selected == null) { el.textContent = "No person selected"; return; }
  el.innerHTML = `Viewing <b>${esc(userName(state.selected))}</b> · ID ${state.selected}`;
}

/* ------------------------------------------------------------
   Home: selected person + recommendations (main feature) + friends
   ------------------------------------------------------------ */
function renderProfile() {
  const u = userById(state.selected);
  if (!u) return;
  const nf = state.friends.length;
  $("profile-card").innerHTML = `
    <div class="avatar ${avatarClass(u.id)}">${esc(initials(u.name))}</div>
    <div class="profile-info">
      <h2>${esc(u.name)}</h2>
      <div class="pid">ID ${u.id}${u.group ? " · " + esc(u.group) + " circle" : ""}</div>
      <div class="badges">
        <span class="badge">${nf} direct friend${nf === 1 ? "" : "s"}</span>
        <span class="badge badge-coral">${state.recs.length} recommendation${state.recs.length === 1 ? "" : "s"}</span>
        ${nf === 0 ? '<span class="badge badge-muted">No friends yet</span>' : ""}
      </div>
    </div>
    <div class="profile-actions">
      <button class="btn" data-goto="network">Open network map</button>
    </div>`;
  $("friends-subtitle").textContent = nf
    ? `${u.name} is directly connected to ${nf} ${nf === 1 ? "person" : "people"}.`
    : "No direct friendships yet, so there is no circle to expand.";
}

function mutualLabel(n) {
  return `${n} mutual friend${n === 1 ? "" : "s"}`;
}

function renderRecommendations() {
  const grid = $("recs-grid");
  const sub = $("recs-subtitle");
  const name = userName(state.selected);

  if (state.selected == null) return;

  if (!state.friends.length) {
    grid.innerHTML = `<div class="empty-box">No recommendations yet.<br>
      ${esc(name)} has no direct friends, so there is no circle of people to expand.</div>`;
    sub.textContent = `${name} has no direct friends.`;
    return;
  }
  if (!state.recs.length) {
    grid.innerHTML = `<div class="empty-box">No suggestions for ${esc(name)}.<br>
      Everyone within two steps is already a direct friend.</div>`;
    sub.textContent = `Everyone two steps from ${name} is already connected.`;
    return;
  }

  sub.textContent = `People two steps from ${name}, best matches first.`;

  /* Rendered in the backend's order — no sorting happens in the browser. */
  grid.innerHTML = state.recs.map((r, i) => `
    <button type="button" class="rec-card${i === 0 ? " is-top" : ""}" data-select="${r.user_id}">
      <span class="rec-rank">#${i + 1}</span>
      <span class="rec-body">
        <span class="rec-name">${esc(userName(r.user_id))}</span>
        <span class="rec-id">ID ${r.user_id}${userGroup(r.user_id) ? " · " + esc(userGroup(r.user_id)) : ""}</span>
        <span class="rec-mutual">${esc(mutualLabel(r.mutual_count))}</span>
        <span class="rec-tag">#${i + 1} Recommendation</span>
      </span>
    </button>`).join("");
}

function friendCardHtml(id, viewerName) {
  return `
    <button type="button" class="friend-card" data-select="${id}">
      <span class="friend-top">
        <span class="avatar avatar-sm ${avatarClass(id)}">${esc(initials(userName(id)))}</span>
        <span>
          <span class="friend-name">${esc(userName(id))}</span>
          <span class="friend-id">ID ${id}${userGroup(id) ? " · " + esc(userGroup(id)) : ""}</span>
        </span>
      </span>
      <span class="friend-note">Directly connected to <b>${esc(viewerName)}</b>.</span>
    </button>`;
}

function chipHtml(id) {
  return `
    <button type="button" class="chip" data-select="${id}">
      <span class="avatar avatar-sm ${avatarClass(id)}">${esc(initials(userName(id)))}</span>
      ${esc(userName(id))}<span class="chip-id">ID ${id}</span>
    </button>`;
}

function renderFriends() {
  const name = userName(state.selected);
  const chips = state.friends.map(chipHtml).join("");

  $("friends-grid").innerHTML = chips ||
    `<span class="chip-empty">No direct friends yet.</span>`;

  $("friends-view-subtitle").textContent = state.friends.length
    ? `${state.friends.length} direct friend${state.friends.length === 1 ? "" : "s"} of ${name}.`
    : `${name} has no direct friends.`;
  $("friends-view-grid").innerHTML = state.friends.map(id => friendCardHtml(id, name)).join("") ||
    `<div class="empty-box">No direct friends yet.</div>`;
}

function renderHowStats() {
  const s = state.summary || {};
  const st = state.recStats;
  $("how-stats").innerHTML = [
    `<span class="badge">${s.users ?? "–"} people in the community</span>`,
    `<span class="badge">${s.friendships ?? "–"} connections</span>`,
    st ? `<span class="badge badge-coral">Last search looked at ${st.edges_scanned ?? "–"} connections</span>` : "",
    st ? `<span class="badge badge-amber">${st.candidates_seen ?? "–"} possible matches considered</span>` : ""
  ].join("");
}

/* ------------------------------------------------------------
   Network map (canvas) + contextual detail panel

   Everything drawn or listed comes from the backend:
     • 1-hop ring = /api/friends?user=ID
     • 2-hop ring = /api/recommend?user=ID&top=64  (full candidate set)
     • edges      = /api/friends?user=ID for every drawn node
   Every node is clickable and hoverable; the detail panel on the
   right always describes the node currently in focus.
   ------------------------------------------------------------ */
const COLORS = {
  sel:    "#1f6feb",
  friend: "#6fa8f5",
  cand:   "#ff6a3d",
  other:  "#cdd8e6",
  edge:   "rgba(92,109,134,.22)",
  edgeHot:"rgba(31,111,235,.85)"
};

function ring(cx, cy, r, ids, offset) {
  const m = new Map();
  ids.forEach((id, i) => {
    const a = (i / Math.max(ids.length, 1)) * 2 * Math.PI - Math.PI / 2 + offset;
    m.set(id, [cx + r * Math.cos(a), cy + r * Math.sin(a)]);
  });
  return m;
}

async function drawNetwork() {
  const cv = $("net-canvas");
  if (!cv || state.selected == null) return;

  const token = state.loadToken;
  const sel = state.selected;
  const dpr = window.devicePixelRatio || 1;
  const cssW = cv.clientWidth || 760;
  const cssH = Math.round(cssW * (480 / 760));
  cv.width = Math.round(cssW * dpr);
  cv.height = Math.round(cssH * dpr);
  cv.style.height = cssH + "px";

  const ctx = cv.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const W = cssW, H = cssH, cx = W / 2, cy = H / 2;
  const R = Math.max(70, Math.min(W, H) / 2 - 44);

  const friends = state.friends.slice();
  const friendSet = new Set(friends);
  const candIds = state.graphRecs.map(r => r.user_id)
    .filter(id => id !== sel && !friendSet.has(id));
  const candSet = new Set(candIds);
  const others = state.users.map(u => u.id)
    .filter(id => id !== sel && !friendSet.has(id) && !candSet.has(id))
    .slice(0, 36);                       // keeps the outside layer readable

  const pos = new Map();
  pos.set(sel, [cx, cy]);
  for (const [id, p] of ring(cx, cy, R * 0.52, friends, 0)) pos.set(id, p);
  for (const [id, p] of ring(cx, cy, R, candIds, 0.3)) pos.set(id, p);
  others.forEach((id, i) => {
    const cols = Math.max(1, Math.min(others.length, Math.floor(W / 48)));
    pos.set(id, [24 + (i % cols) * ((W - 48) / Math.max(cols - 1, 1)),
                 H - 16 - Math.floor(i / cols) * 20]);
  });

  /* Real adjacency for the drawn nodes (fetched, then cached). */
  const seen = new Set(), edges = [];
  for (const id of [sel, ...friends, ...candIds]) {
    let adj = state.adjacency.get(id);
    if (!adj) {
      try { adj = await API.getFriends(id); } catch (e) { adj = []; }
      state.adjacency.set(id, adj);
    }
    for (const other of adj) {
      if (!pos.has(other)) continue;
      const key = id < other ? `${id}-${other}` : `${other}-${id}`;
      if (seen.has(key)) continue;
      seen.add(key);
      edges.push([id, other]);
    }
  }
  if (token !== state.loadToken) return;   // a newer selection won the race

  const nodes = [];
  const drawNode = (id, fill, r, label, kind) => {
    const [x, y] = pos.get(id);
    nodes.push({ id, x, y, r, kind });
  };
  others.forEach(id => drawNode(id, COLORS.other, 6, "", "other"));
  candIds.forEach(id => drawNode(id, COLORS.cand, 10, String(id), "cand"));
  friends.forEach(id => drawNode(id, COLORS.friend, 12, String(id), "friend"));
  drawNode(sel, COLORS.sel, 16, userName(sel).split(" ")[0], "sel");

  const paint = () => {
    ctx.clearRect(0, 0, W, H);

    for (const [a, b] of edges) {
      const [x1, y1] = pos.get(a), [x2, y2] = pos.get(b);
      const hot = a === sel || b === sel;
      const touchingHover = state.hover != null && (a === state.hover || b === state.hover);
      ctx.strokeStyle = hot ? COLORS.edgeHot : touchingHover ? "rgba(255,106,61,.5)" : COLORS.edge;
      ctx.lineWidth = hot ? 2 : touchingHover ? 1.6 : 1;
      ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke();
    }

    const hovered = state.hover;
    const nodes2 = nodes.slice().sort((a, b) => a.r - b.r);   // big nodes on top
    for (const n of nodes2) {
      const isSel = n.kind === "sel";
      const isHover = hovered === n.id;
      const r = isHover ? n.r + 3 : n.r;

      if (isSel || isHover) {
        ctx.beginPath(); ctx.arc(n.x, n.y, r + 6, 0, Math.PI * 2);
        ctx.strokeStyle = isSel ? "rgba(31,111,235,.28)" : "rgba(255,106,61,.30)";
        ctx.lineWidth = isSel ? 6 : 5; ctx.stroke();
      }
      ctx.beginPath(); ctx.arc(n.x, n.y, r, 0, Math.PI * 2);
      ctx.fillStyle = isSel ? COLORS.sel : isHover ? "#ff8a63"
        : n.kind === "friend" ? COLORS.friend : n.kind === "cand" ? COLORS.cand : COLORS.other;
      ctx.fill();
      if (isSel) { ctx.strokeStyle = "#fff"; ctx.lineWidth = 2.5; ctx.stroke(); }

      const showLabel = n.kind !== "other" || isHover;
      if (showLabel) {
        ctx.fillStyle = isSel ? "#14243b" : "#5c6d86";
        ctx.font = (isSel ? "600 12px " : "11px ") + "'Segoe UI', sans-serif";
        ctx.textAlign = "center";
        const text = isSel ? userName(n.id).split(" ")[0]
          : isHover ? userName(n.id) : String(n.id);
        ctx.fillText(text, n.x, n.y + r + 14);
      }
    }
  };

  cv._hits = nodes;
  cv._paint = paint;
  paint();
}

/* Hit-test helper: topmost node under a CSS-pixel point. */
function nodeAt(x, y) {
  const hits = $("net-canvas")._hits || [];
  return hits.slice().sort((a, b) => b.r - a.r)
    .find(n => (x - n.x) ** 2 + (y - n.y) ** 2 <= (n.r + 5) ** 2) || null;
}

function bindCanvas() {
  const cv = $("net-canvas");
  const tip = $("node-tip");

  cv.addEventListener("mousemove", (e) => {
    const rect = cv.getBoundingClientRect();
    const x = e.clientX - rect.left, y = e.clientY - rect.top;
    const hit = nodeAt(x, y);
    const id = hit ? hit.id : null;
    cv.classList.toggle("is-pointing", !!hit);
    if (id !== state.hover) {
      state.hover = id;
      if (cv._paint) cv._paint();
    }
    if (hit) {
      tip.classList.remove("hidden");
      tip.textContent = userName(hit.id);
      tip.style.left = x + "px";
      tip.style.top = y + "px";
    } else {
      tip.classList.add("hidden");
    }
  });

  cv.addEventListener("mouseleave", () => {
    state.hover = null;
    if (cv._paint) cv._paint();
    tip.classList.add("hidden");
    cv.classList.remove("is-pointing");
  });

  cv.addEventListener("click", (e) => {
    const rect = cv.getBoundingClientRect();
    const hit = nodeAt(e.clientX - rect.left, e.clientY - rect.top);
    if (!hit || hit.id === state.selected) return;
    selectUser(hit.id);
  });
}

/* Right-hand contextual panel for the node in focus. */
function renderDetailPanel(err) {
  const id = state.selected;
  const body = $("detail-body"), empty = $("detail-empty");
  if (id == null) return;
  if (err) {
    body.classList.add("hidden");
    empty.classList.remove("hidden");
    empty.className = "detail-empty";
    empty.innerHTML = `<span class="detail-error">${esc(err.message)}</span>`;
    return;
  }

  empty.classList.add("hidden");
  body.classList.remove("hidden");

  $("dp-avatar").textContent = initials(userName(id));
  $("dp-avatar").className = `avatar avatar-lg ${avatarClass(id)}`;
  $("dp-name").textContent = userName(id);
  $("dp-id").textContent = "ID " + id + (userGroup(id) ? " · " + userGroup(id) : "");

  $("dp-friend-count").textContent = state.friends.length;
  $("dp-friends").innerHTML = state.friends.map(chipHtml).join("") ||
    `<span class="detail-none">No direct friends yet.</span>`;

  $("dp-rec-count").textContent = state.recs.length;
  $("dp-recs").innerHTML = state.recs.length
    ? state.recs.map((r, i) => `
        <li data-select="${r.user_id}">
          <span class="dr-rank">#${i + 1}</span>
          <span class="dr-name">${esc(userName(r.user_id))}</span>
          <span class="dr-mutual">${esc(mutualLabel(r.mutual_count))}</span>
        </li>`).join("")
    : `<span class="detail-none">${
        state.friends.length
          ? "Everyone two steps away is already a direct friend."
          : "No direct friends, so there is nobody to suggest yet."
      }</span>`;
}

/* ------------------------------------------------------------
   Loading + navigation
   ------------------------------------------------------------ */
function setView(v) {
  state.view = v;
  document.querySelectorAll(".nav-link").forEach(b =>
    b.classList.toggle("active", b.dataset.view === v));
  document.querySelectorAll(".view").forEach(s =>
    s.classList.toggle("active", s.id === `view-${v}`));
  if (v === "network") drawNetwork();
  if (v === "how")    renderHowStats();
}

async function loadRecommendations(token) {
  const topN = parseInt($("rec-top").value, 10);
  try {
    const payload = await API.getRecommendations(state.selected, topN);
    if (token !== state.loadToken) return;
    state.recs = Array.isArray(payload.recommendations) ? payload.recommendations : [];
    state.recStats = payload.stats || null;
    return null;
  } catch (e) {
    if (token !== state.loadToken) return null;
    state.recs = [];
    state.recStats = null;
    return e;
  }
}

async function selectUser(userId, opts) {
  const quiet = opts && opts.quiet;
  const token = ++state.loadToken;
  state.selected = Number(userId);
  state.recs = [];
  state.friends = [];
  state.hover = null;

  $("user-select").value = String(state.selected);
  renderTopbar();
  $("recs-grid").innerHTML = `<div class="placeholder">Finding the best matches…</div>`;
  $("friends-grid").innerHTML = `<span class="chip-empty">Loading friends…</span>`;
  $("detail-empty").innerHTML = `Select a node on the map to see details.`;
  $("detail-body").classList.add("hidden");
  $("detail-empty").classList.remove("hidden");

  let friends;
  try {
    friends = await API.getFriends(state.selected);
  } catch (e) {
    if (token !== state.loadToken) return;
    errorBox($("recs-grid"), e);
    $("friends-grid").innerHTML = "";
    errorBox($("friends-view-grid"), e);
    renderDetailPanel(e);
    return;
  }
  if (token !== state.loadToken) return;

  state.friends = friends.slice();
  state.adjacency.set(state.selected, state.friends.slice());
  renderProfile();
  renderFriends();

  /* Full candidate set (top = GRAPH_TOP) used only to colour the map. */
  try {
    const full = await API.getRecommendations(state.selected, GRAPH_TOP);
    if (token !== state.loadToken) return;
    state.graphRecs = Array.isArray(full.recommendations) ? full.recommendations : [];
  } catch (e) {
    if (token === state.loadToken) state.graphRecs = [];
  }
  if (token !== state.loadToken) return;

  const recErr = await loadRecommendations(token);
  if (token !== state.loadToken) return;

  if (recErr) {
    errorBox($("recs-grid"), recErr);
    $("recs-subtitle").textContent = "";
  } else {
    renderRecommendations();
  }
  renderProfile();
  renderDetailPanel();
  renderHowStats();
  if (state.view === "network") await drawNetwork();
  if (state.view === "how") renderHowStats();

  if (!quiet) {
    toast(`${userName(state.selected)} · ${state.recs.length} recommendation${state.recs.length === 1 ? "" : "s"}`);
  }
}

/* Search dropdown */
function wireSearch() {
  const input = $("search"), box = $("search-results");
  input.addEventListener("input", () => {
    const q = input.value.trim().toLowerCase();
    if (!q) { box.classList.add("hidden"); return; }
    const hits = state.users.filter(u =>
      String(u.name).toLowerCase().includes(q) || String(u.id) === q.replace(/^#/, ""));
    box.innerHTML = hits.length
      ? hits.slice(0, 12).map(u => `
          <div class="search-item" data-select="${u.id}">
            <div class="avatar avatar-sm ${avatarClass(u.id)}">${esc(initials(u.name))}</div>
            <span>${esc(u.name)}</span><span class="si-id">ID ${u.id}</span>
          </div>`).join("")
      : `<div class="search-item">No match for “${esc(q)}”</div>`;
    box.classList.remove("hidden");
  });
  document.addEventListener("click", (e) => {
    if (!$("search-wrap").contains(e.target)) box.classList.add("hidden");
  });
}

/* One delegated click handler for data-* actions */
function wireActions() {
  document.addEventListener("click", (e) => {
    const el = e.target.closest("[data-select],[data-goto]");
    if (!el) return;
    if (el.dataset.select !== undefined) {
      $("search-results").classList.add("hidden");
      $("search").value = "";
      selectUser(parseInt(el.dataset.select, 10), { quiet: true });
    } else if (el.dataset.goto) {
      setView(el.dataset.goto);
    }
  });
  document.querySelectorAll(".nav-link").forEach(b =>
    b.addEventListener("click", () => setView(b.dataset.view)));

  $("btn-recommend").addEventListener("click", async () => {
    const token = state.loadToken;
    $("recs-grid").innerHTML = `<div class="placeholder">Finding the best matches…</div>`;
    const err = await loadRecommendations(token);
    if (err) { errorBox($("recs-grid"), err); return; }
    renderRecommendations();
    renderProfile();
    renderDetailPanel();
    renderHowStats();
    if (state.view === "network") drawNetwork();
  });
  $("rec-top").addEventListener("change", () => $("btn-recommend").click());
  $("user-select").addEventListener("change", e => selectUser(e.target.value, { quiet: true }));
  bindCanvas();
  window.addEventListener("resize", () => { if (state.view === "network") drawNetwork(); });
}

/* ------------------------------------------------------------
   Init
   ------------------------------------------------------------ */
(async function init() {
  try {
    const [users, summary] = await Promise.all([API.getUsers(), API.getSummary()]);
    state.users = users;
    state.summary = summary;
  } catch (e) {
    errorBox($("recs-grid"), e);
    errorBox($("friends-grid"), e);
    toast("Backend unreachable — is the server running?");
    return;
  }

  renderStats();
  renderUserSelect();
  wireSearch();
  wireActions();
  setView("home");

  if (state.users.length) await selectUser(state.users[0].id, { quiet: true });
})();
