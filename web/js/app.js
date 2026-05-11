/* global fetch */

const $ = (sel) => document.querySelector(sel);

const flash = (msg, type) => {
  const el = $("#flash");
  if (!el) return;
  el.textContent = msg;
  el.className = "flash show " + (type === "ok" ? "ok" : "err");
  clearTimeout(flash._t);
  flash._t = setTimeout(() => el.classList.remove("show"), 4200);
};

async function api(method, path, body, isText) {
  const opts = { method, credentials: "same-origin" };
  if (body !== undefined) {
    opts.body = body;
    opts.headers = { "Content-Type": isText ? "text/plain; charset=utf-8" : "application/json" };
  }
  const res = await fetch(path, opts);
  const ct = res.headers.get("content-type") || "";
  if (!res.ok) {
    const t = ct.includes("json") ? JSON.stringify(await res.json().catch(() => ({}))) : await res.text();
    throw new Error(t || res.statusText);
  }
  if (ct.includes("application/json")) return res.json();
  return res.text();
}

let state = { mode: "guest", user: "", canRead: false, canWrite: false, priority: 0 };

function setNav(view) {
  document.querySelectorAll(".nav-btn").forEach((b) => {
    b.classList.toggle("active", b.dataset.view === view);
  });
  document.querySelectorAll("[data-panel]").forEach((p) => {
    p.style.display = p.dataset.panel === view ? "block" : "none";
  });
}

function applyState() {
  const guest = state.mode === "guest";
  const loginEl = $("#loginView");
  const appEl = $("#appShell");
  if (loginEl) loginEl.classList.toggle("hidden", !guest);
  if (appEl) appEl.classList.toggle("hidden", guest);

  if (guest) return;

  const layout = document.querySelector(".layout");
  if (layout) {
    layout.classList.toggle("owner", state.mode === "owner");
  }

  $("#pillMode").textContent = state.mode === "owner" ? "Administrator" : "User";
  $("#pillMode").className = "pill " + (state.mode === "owner" ? "owner" : "user");
  $("#pillUser").textContent = state.user || "—";

  const readBtn = $("#btnReload");
  const saveBtn = $("#btnSave");
  const ta = $("#docBody");
  readBtn.disabled = !state.canRead;
  saveBtn.disabled = !state.canWrite;
  ta.readOnly = !state.canWrite;
  ta.title = state.canWrite ? "Edit and save allowed." : state.canRead ? "Read only." : "No access.";

  document.querySelectorAll(".owner-only").forEach((el) => {
    el.style.display = state.mode === "owner" ? "" : "none";
  });
}

async function bootstrap() {
  state = await api("GET", "/api/bootstrap");
  applyState();
}

async function loginOwner() {
  try {
    await api("POST", "/api/auth/login", JSON.stringify({ role: "owner" }));
    await bootstrap();
    setNav("doc");
    await loadDocument();
    flash("Admin signed in.", "ok");
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function loginUser() {
  const u = $("#loginUsername").value.trim();
  if (!u) return flash("Username likho.", "err");
  try {
    await api("POST", "/api/auth/login", JSON.stringify({ role: "user", username: u }));
    await bootstrap();
    setNav("doc");
    await loadDocument();
    flash("Signed in as " + u, "ok");
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function logout() {
  try {
    await api("POST", "/api/auth/logout", "{}");
  } catch (e) {
    /* ignore */
  }
  state = { mode: "guest", user: "", canRead: false, canWrite: false, priority: 0 };
  applyState();
  flash("Account se bahar aa gaye. Dobara sign in karo.", "ok");
}

async function loadDocument() {
  if (state.mode === "guest" || !state.canRead) return;
  try {
    const text = await api("GET", "/api/document");
    $("#docBody").value = text;
    flash("Document load ho gaya.", "ok");
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function saveDocument() {
  if (!state.canWrite) return flash("Write permission nahi.", "err");
  try {
    await api("PUT", "/api/document", $("#docBody").value, true);
    flash("Save ho gaya (write lock).", "ok");
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function loadUsers() {
  if (state.mode !== "owner") return;
  try {
    const users = await api("GET", "/api/users");
    const tbody = $("#usersBody");
    tbody.innerHTML = "";
    users.forEach((u) => {
      const tr = document.createElement("tr");
      tr.innerHTML = `<td>${escapeHtml(u.name)}</td><td>${escapeHtml(u.priority)}</td><td>${escapeHtml(
        u.access
      )}</td><td>${u.pid}</td><td>${u.active ? "yes" : "no"}</td>`;
      tbody.appendChild(tr);
    });
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

async function addUser(ev) {
  ev.preventDefault();
  if (state.mode !== "owner") return;
  const name = $("#newName").value.trim();
  const priority = parseInt($("#newPri").value, 10);
  const access = parseInt($("#newAcc").value, 10);
  try {
    await api("POST", "/api/users/add", JSON.stringify({ name, priority, access }));
    flash("User add ho gaya.", "ok");
    $("#addUserForm").reset();
    await loadUsers();
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function removeUser() {
  const name = $("#rmName").value.trim();
  if (!name) return flash("Username likho.", "err");
  try {
    await api("POST", "/api/users/remove", JSON.stringify({ name }));
    flash("User hata diya.", "ok");
    await loadUsers();
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function updateUser(ev) {
  ev.preventDefault();
  const name = $("#upName").value.trim();
  const priority = parseInt($("#upPri").value, 10);
  const access = parseInt($("#upAcc").value, 10);
  try {
    await api("POST", "/api/users/update", JSON.stringify({ name, priority, access }));
    flash("Update ho gaya.", "ok");
    await loadUsers();
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function pushHistory() {
  try {
    await api("POST", "/api/history/push", "{}");
    flash("Snapshot push.", "ok");
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function popHistory() {
  try {
    await api("POST", "/api/history/pop", "{}");
    flash("Pop ho gaya.", "ok");
    await loadDocument();
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

async function loadHistory() {
  try {
    const txt = await api("GET", "/api/history");
    $("#historyOut").textContent = txt || "(empty)";
  } catch (e) {
    flash(e.message || String(e), "err");
  }
}

function wire() {
  $("#btnLoginOwner").addEventListener("click", loginOwner);
  $("#btnLoginUser").addEventListener("click", loginUser);
  $("#btnLogout").addEventListener("click", logout);

  $("#btnReload").addEventListener("click", loadDocument);
  $("#btnSave").addEventListener("click", saveDocument);

  document.querySelectorAll(".nav-btn").forEach((b) => {
    b.addEventListener("click", () => {
      if (b.disabled) return;
      setNav(b.dataset.view);
      if (b.dataset.view === "users") loadUsers();
      if (b.dataset.view === "history") loadHistory();
    });
  });

  $("#addUserForm").addEventListener("submit", addUser);
  $("#btnRemoveUser").addEventListener("click", removeUser);
  $("#updateUserForm").addEventListener("submit", updateUser);
  $("#btnPushHist").addEventListener("click", pushHistory);
  $("#btnPopHist").addEventListener("click", popHistory);
  $("#btnRefreshHist").addEventListener("click", loadHistory);

  $("#loginUsername").addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") loginUser();
  });
}

(async function init() {
  wire();
  try {
    await bootstrap();
    if (state.mode !== "guest") {
      setNav("doc");
      await loadDocument();
    }
  } catch (e) {
    flash("Server se connect nahi ho saka: " + (e.message || e), "err");
  }
})();
