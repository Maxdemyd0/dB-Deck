import { ReactNode, RefObject, useEffect, useLayoutEffect, useRef, useState } from "react";
import { dismissToast, useToasts } from "../engine/store";
import { Icon } from "./Icon";

export function Modal({ title, children, onClose, wide, actions }: { title: string; children: ReactNode; onClose: () => void; wide?: boolean; actions?: ReactNode }) {
  useEffect(() => {
    const k = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    window.addEventListener("keydown", k);
    return () => window.removeEventListener("keydown", k);
  }, [onClose]);
  return (
    <div className="overlay" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className={`modal${wide ? " wide" : ""}`} role="dialog" aria-modal="true" aria-label={title}>
        <div className="row">
          <h2 className="grow">{title}</h2>
          <button className="btn ghost icon small" onClick={onClose} aria-label="Close">
            <Icon name="close" />
          </button>
        </div>
        {children}
        {actions && <div className="modal-actions">{actions}</div>}
      </div>
    </div>
  );
}

export interface MenuItem {
  label: string;
  onClick?: () => void;
  danger?: boolean;
  disabled?: boolean;
  separator?: boolean;
  heading?: boolean;
  checked?: boolean;
  /** Color square shown before the label (strip color picker). */
  swatch?: string;
}

/** Strip colors offered in channel and bus menus. */
export const STRIP_COLORS: { name: string; value: string }[] = [
  { name: "Red", value: "#ef4444" },
  { name: "Orange", value: "#f97316" },
  { name: "Yellow", value: "#eab308" },
  { name: "Green", value: "#22c55e" },
  { name: "Teal", value: "#14b8a6" },
  { name: "Blue", value: "#3b82f6" },
  { name: "Purple", value: "#a855f7" },
  { name: "Pink", value: "#ec4899" },
];

/** Menu items for choosing a strip color. */
export function colorMenuItems(current: string | undefined, set: (color: string) => void): MenuItem[] {
  return [
    { heading: true, label: "Color" },
    { label: "None", checked: !current, onClick: () => set("") },
    ...STRIP_COLORS.map((c) => ({ label: c.name, swatch: c.value, checked: current === c.value, onClick: () => set(c.value) })),
  ];
}

/**
 * Popup menu anchored to a button. With `contextTarget`, right-clicking that
 * element opens the same menu at the pointer.
 */
export function MenuButton({ items, label = "More", icon = "more", className = "btn ghost icon small", contextTarget }: { items: MenuItem[]; label?: string; icon?: string; className?: string; contextTarget?: RefObject<HTMLElement | null> }) {
  const [pos, setPos] = useState<{ x: number; y: number } | null>(null);
  const btn = useRef<HTMLButtonElement>(null);
  const menu = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    if (!pos || !menu.current) return;
    const r = menu.current.getBoundingClientRect();
    if (r.right > window.innerWidth - 8 || r.bottom > window.innerHeight - 8)
      setPos({ x: Math.min(pos.x, window.innerWidth - r.width - 8), y: Math.min(pos.y, window.innerHeight - r.height - 8) });
  }, [pos]);
  useEffect(() => {
    if (!pos) return;
    const close = (e: MouseEvent) => !menu.current?.contains(e.target as Node) && setPos(null);
    const key = (e: KeyboardEvent) => e.key === "Escape" && setPos(null);
    window.addEventListener("mousedown", close);
    window.addEventListener("keydown", key);
    return () => {
      window.removeEventListener("mousedown", close);
      window.removeEventListener("keydown", key);
    };
  }, [pos]);
  useEffect(() => {
    const el = contextTarget?.current;
    if (!el) return;
    const open = (e: MouseEvent) => {
      if ((e.target as HTMLElement).closest("input, textarea")) return;
      e.preventDefault();
      setPos({ x: e.clientX, y: e.clientY });
    };
    el.addEventListener("contextmenu", open);
    return () => el.removeEventListener("contextmenu", open);
  }, [contextTarget]);
  return (
    <>
      <button
        ref={btn}
        className={className}
        aria-label={label}
        title={label}
        onClick={() => {
          const r = btn.current!.getBoundingClientRect();
          setPos(pos ? null : { x: r.left, y: r.bottom + 4 });
        }}
      >
        <Icon name={icon} />
      </button>
      {pos && (
        <div ref={menu} className="menu" style={{ left: pos.x, top: pos.y }} role="menu">
          {items.map((it, i) =>
            it.separator ? (
              <hr key={i} />
            ) : it.heading ? (
              <div key={i} className="menu-label">
                {it.label}
              </div>
            ) : (
              <button
                key={i}
                role="menuitem"
                className={it.danger ? "danger" : ""}
                disabled={it.disabled}
                onClick={() => {
                  setPos(null);
                  it.onClick?.();
                }}
              >
                <span style={{ width: 14, display: "inline-flex" }}>{it.checked && <Icon name="check" size={14} />}</span>
                {it.swatch && <span className="swatch" style={{ background: it.swatch }} />}
                {it.label}
              </button>
            ),
          )}
        </div>
      )}
    </>
  );
}

export function Toasts() {
  const toasts = useToasts();
  return (
    <div className="toasts" aria-live="polite">
      {toasts.map((t) => (
        <div key={t.id} className={`toast ${t.kind}`}>
          <Icon name={t.kind === "info" ? "info" : "warn"} />
          <div className="grow">{t.text}</div>
          <button className="btn ghost icon small" onClick={() => dismissToast(t.id)} aria-label="Dismiss">
            <Icon name="close" size={14} />
          </button>
        </div>
      ))}
    </div>
  );
}

export function Prompt({ title, label, initial, onSubmit, onClose, submitLabel = "Save" }: { title: string; label: string; initial: string; onSubmit: (v: string) => void; onClose: () => void; submitLabel?: string }) {
  const [v, setV] = useState(initial);
  const submit = () => {
    if (!v.trim()) return;
    onSubmit(v.trim());
    onClose();
  };
  return (
    <Modal
      title={title}
      onClose={onClose}
      actions={
        <>
          <button className="btn" onClick={onClose}>Cancel</button>
          <button className="btn primary" onClick={submit} disabled={!v.trim()}>{submitLabel}</button>
        </>
      }
    >
      <label className="col" style={{ marginTop: 10 }}>
        <span className="muted small">{label}</span>
        <input type="text" autoFocus value={v} onChange={(e) => setV(e.target.value)} onKeyDown={(e) => e.key === "Enter" && submit()} />
      </label>
    </Modal>
  );
}
