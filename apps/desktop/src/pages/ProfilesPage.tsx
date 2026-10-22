import { useState } from "react";
import { Icon } from "../components/Icon";
import { Confirm, Modal, Prompt } from "../components/Overlay";
import { deleteWithUndo, request } from "../engine/store";
import type { EngineState } from "../engine/types";

interface ImportResult {
  profileId: string;
  profileName: string;
  edition: string;
  source: string;
  channels: number;
  buses: number;
  notes: string[];
}

type PromptState = { title: string; initial: string; submit: string; run: (v: string) => void } | null;

export function ProfilesPage({ state }: { state: EngineState }) {
  const [prompt, setPrompt] = useState<PromptState>(null);
  const [imported, setImported] = useState<ImportResult | null>(null);
  const [importing, setImporting] = useState(false);
  const [deleting, setDeleting] = useState<{ id: string; name: string } | null>(null);
  const runImport = () => {
    setImporting(true);
    request<ImportResult>("importVoicemeeter")
      .then(setImported)
      .catch(() => {})
      .finally(() => setImporting(false));
  };
  const active = state.activeProfileId;
  const scenes = state.profile.scenes;
  return (
    <div className="page">
      <h1 className="page-title">Profiles &amp; Scenes</h1>
      <p className="page-sub">
        A <strong>profile</strong> is a complete mixer setup — channels, groups, buses, routing, effects and outputs. A <strong>scene</strong> is a quick snapshot of levels, mutes and send
        levels inside the current profile, and switches without restarting any device.
      </p>
      <div className="grid2">
        <div className="card">
          <div className="row" style={{ marginBottom: 6 }}>
            <h3 className="grow" style={{ margin: 0 }}>Profiles</h3>
            <button className="btn small" onClick={() => setPrompt({ title: "New profile", initial: "Streaming", submit: "Create", run: (name) => request("createProfile", { name }).catch(() => {}) })}>
              <Icon name="plus" size={12} /> New
            </button>
            <button className="btn small" onClick={() => setPrompt({ title: "Duplicate current profile", initial: `${state.profile.name} copy`, submit: "Duplicate", run: (name) => request("createProfile", { name, copyCurrent: true }).catch(() => {}) })}>
              Duplicate
            </button>
            <button className="btn small" disabled={importing} title="Create a profile from your Voicemeeter settings (Documents\Voicemeeter)" onClick={runImport}>
              {importing ? "Importing…" : "Import from Voicemeeter"}
            </button>
          </div>
          <div className="list">
            {state.profiles.map((p) => (
              <div className="list-row" key={p.id}>
                <span className={`dot ${p.id === active ? "ok" : ""}`} />
                <span className="grow ellipsis">{p.name}</span>
                {p.id === active ? <span className="badge accent">Active</span> : <button className="btn small primary" onClick={() => request("loadProfile", { id: p.id }).catch(() => {})}>Load</button>}
                <button className="btn small ghost" onClick={() => setPrompt({ title: "Rename profile", initial: p.name, submit: "Rename", run: (name) => request("renameProfile", { id: p.id, name }).catch(() => {}) })}>Rename</button>
                <button className="btn small ghost danger" disabled={p.id === active || state.profiles.length === 1} title={p.id === active ? "Load another profile first" : undefined} onClick={() => setDeleting({ id: p.id, name: p.name })}>Delete</button>
              </div>
            ))}
          </div>
        </div>
        <div className="card">
          <div className="row" style={{ marginBottom: 6 }}>
            <h3 className="grow" style={{ margin: 0 }}>Scenes in “{state.profile.name}”</h3>
            <button className="btn small" onClick={() => setPrompt({ title: "Save current levels as a scene", initial: "Late Night", submit: "Save", run: (name) => request("createScene", { name }).catch(() => {}) })}>
              <Icon name="plus" size={12} /> Save current
            </button>
          </div>
          {scenes.length === 0 && <span className="faint">No scenes yet. Set your levels, then save them as a scene.</span>}
          <div className="list">
            {scenes.map((s) => (
              <div className="list-row" key={s.id}>
                <Icon name="scene" />
                <span className="grow ellipsis">{s.name}</span>
                <button className="btn small primary" onClick={() => request("applyScene", { id: s.id }).catch(() => {})}>Apply</button>
                <button className="btn small ghost" title="Replace this scene with the current levels" onClick={() => request("updateScene", { id: s.id, recapture: true }).catch(() => {})}>Update</button>
                <button className="btn small ghost" onClick={() => setPrompt({ title: "Rename scene", initial: s.name, submit: "Rename", run: (name) => request("updateScene", { id: s.id, name }).catch(() => {}) })}>Rename</button>
                <button className="btn small ghost danger" onClick={() => void deleteWithUndo("deleteScene", { id: s.id }, `scene "${s.name}"`)}>Delete</button>
              </div>
            ))}
          </div>
        </div>
      </div>
      {deleting && (
        <Confirm
          title="Delete profile?"
          text={<>“{deleting.name}” and all its channels, buses, routing and scenes will be deleted. This can't be undone.</>}
          confirmLabel="Delete profile"
          onConfirm={() => void request("deleteProfile", { id: deleting.id }).catch(() => {})}
          onClose={() => setDeleting(null)}
        />
      )}
      {imported && (
        <Modal
          title={`Imported "${imported.profileName}"`}
          onClose={() => setImported(null)}
          wide
          actions={
            <>
              <button className="btn" onClick={() => setImported(null)}>Keep current profile</button>
              <button className="btn primary" onClick={() => request("loadProfile", { id: imported.profileId }).then(() => setImported(null)).catch(() => {})}>
                Load it now
              </button>
            </>
          }
        >
          <p className="muted small" style={{ marginTop: 4 }}>
            {imported.channels} channels and {imported.buses} buses from Voicemeeter {imported.edition}
            <span className="faint"> ({imported.source})</span>.
          </p>
          <ul style={{ margin: 0, paddingLeft: 18, lineHeight: 1.6 }}>
            {imported.notes.map((n, i) => <li key={i}>{n}</li>)}
          </ul>
        </Modal>
      )}
      {prompt && <Prompt title={prompt.title} label="Name" initial={prompt.initial} submitLabel={prompt.submit} onSubmit={prompt.run} onClose={() => setPrompt(null)} />}
    </div>
  );
}
