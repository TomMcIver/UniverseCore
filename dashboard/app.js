const statePath = 'data/project-status.json';

let dashboardState = null;
let activeFilter = 'all';

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) {
    el.textContent = text;
  }
}

function statusClass(status) {
  return ['complete', 'active', 'planned', 'future'].includes(status) ? status : 'planned';
}

function createProgressRow(label, value) {
  const row = document.createElement('div');
  row.className = 'progress-row';

  const labelEl = document.createElement('div');
  labelEl.className = 'progress-label';
  labelEl.textContent = label;

  const track = document.createElement('div');
  track.className = 'progress-track';

  const fill = document.createElement('div');
  fill.className = 'progress-fill';
  fill.style.width = `${Math.max(0, Math.min(100, Number(value) || 0))}%`;
  track.appendChild(fill);

  const pct = document.createElement('div');
  pct.textContent = `${value}%`;

  row.append(labelEl, track, pct);
  return row;
}

function renderProgress(progress) {
  const container = document.getElementById('progressContainer');
  container.innerHTML = '';

  const labels = [
    ['Simulation foundation', progress.simulation_foundation_percent],
    ['Bronze Age simulator', progress.bronze_age_simulator_percent],
    ['Playable vertical slice', progress.playable_vertical_slice_percent],
    ['Full game vision', progress.full_game_vision_percent]
  ];

  labels.forEach(([label, value]) => container.appendChild(createProgressRow(label, value)));
  setText('progressNote', progress.note || '');
}

function renderTasks(id, tasks) {
  const list = document.getElementById(id);
  list.innerHTML = '';
  tasks.forEach(task => {
    const li = document.createElement('li');
    li.textContent = task;
    list.appendChild(li);
  });
}

function passesFilter(status) {
  if (activeFilter === 'all') {
    return true;
  }
  return status === activeFilter;
}

function renderMilestones() {
  const tbody = document.getElementById('milestoneTable');
  tbody.innerHTML = '';

  dashboardState.milestones
    .filter(m => passesFilter(m.status))
    .forEach(m => {
      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td>${m.id}</td>
        <td>${m.name}</td>
        <td><span class="status ${statusClass(m.status)}">${m.status}</span></td>
        <td>${m.percent}%</td>
      `;
      tbody.appendChild(tr);
    });
}

function renderCommits(commits) {
  const tbody = document.getElementById('commitTable');
  tbody.innerHTML = '';
  commits.forEach(commit => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${commit.hash}</td>
      <td>${commit.date}</td>
      <td>${commit.summary}</td>
    `;
    tbody.appendChild(tr);
  });
}

function installFilters() {
  document.querySelectorAll('.filter').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.filter').forEach(x => x.classList.remove('active'));
      btn.classList.add('active');
      activeFilter = btn.dataset.filter;
      renderMilestones();
    });
  });
}

function render(data) {
  dashboardState = data;

  setText('projectName', data.project);
  setText('phaseLine', `${data.current_phase} • as of ${data.as_of}`);
  setText('currentTask', data.current_task);
  setText('nextTask', data.next_task);
  setText('buildStatus', `${data.build.status} • ${data.build.last_verified} • ${data.build.canonical_workflow}`);
  setText('latestCommit', data.recent_verified_commits?.[0]?.hash || 'n/a');

  renderProgress(data.progress || {});
  renderTasks('completedTasks', data.tasks?.completed || []);
  renderTasks('activeTasks', data.tasks?.active || []);
  renderTasks('plannedTasks', data.tasks?.planned || []);
  renderTasks('invariants', data.invariants || []);
  renderMilestones();
  renderCommits(data.recent_verified_commits || []);
}

async function init() {
  try {
    const response = await fetch(statePath, { cache: 'no-store' });
    if (!response.ok) {
      throw new Error(`Failed to load ${statePath}: ${response.status}`);
    }
    const data = await response.json();
    installFilters();
    render(data);
  } catch (error) {
    setText('phaseLine', 'Failed to load dashboard data');
    console.error(error);
  }
}

init();
