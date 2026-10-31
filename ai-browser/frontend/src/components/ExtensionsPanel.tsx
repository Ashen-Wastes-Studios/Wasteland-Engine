import type { Extension } from "../extensions";

export function ExtensionsPanel({
  extensions,
  onToggle,
  onRemove,
}: {
  extensions: Extension[];
  onToggle: (id: string) => void;
  onRemove: (id: string) => void;
}) {
  return (
    <div style={{ padding: 12 }}>
      <h3 style={{ margin: "0 0 12px" }}>Extensions</h3>
      {extensions.length === 0 && (
        <p style={{ fontSize: 12, color: "var(--muted-foreground)" }}>No extensions installed.</p>
      )}
      {extensions.map((ext) => (
        <div
          key={ext.id}
          style={{
            padding: "8px 6px",
            borderBottom: "1px solid var(--border)",
            fontSize: 12,
            display: "flex",
            justifyContent: "space-between",
            alignItems: "center",
          }}
        >
          <div>
            <div style={{ fontWeight: 600 }}>{ext.name}</div>
            <div style={{ color: "var(--muted-foreground)" }}>{ext.description}</div>
            <div style={{ color: "var(--muted-foreground)", fontSize: 11 }}>
              {ext.contentScripts.length} content script{ext.contentScripts.length === 1 ? "" : "s"}
            </div>
          </div>
          <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
            <label style={{ display: "flex", alignItems: "center", gap: 4, cursor: "pointer" }}>
              <input
                type="checkbox"
                checked={ext.enabled}
                onChange={() => onToggle(ext.id)}
              />
              <span style={{ fontSize: 11 }}>{ext.enabled ? "On" : "Off"}</span>
            </label>
            <button
              onClick={() => onRemove(ext.id)}
              style={{
                padding: "2px 6px",
                background: "transparent",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 4,
                fontSize: 11,
              }}
            >
              Remove
            </button>
          </div>
        </div>
      ))}
    </div>
  );
}
