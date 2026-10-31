import { useState } from "react";
import type { Extension } from "../extensions";
import { ExtensionInstaller } from "./ExtensionInstaller";

export function ExtensionsPanel({
  extensions,
  onToggle,
  onRemove,
  onInstall,
}: {
  extensions: Extension[];
  onToggle: (id: string) => void;
  onRemove: (id: string) => void;
  onInstall: (ext: Extension) => void;
}) {
  const [expanded, setExpanded] = useState<string | null>(null);

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
          }}
        >
          <div
            style={{
              display: "flex",
              justifyContent: "space-between",
              alignItems: "center",
              cursor: "pointer",
            }}
            onClick={() => setExpanded((prev) => (prev === ext.id ? null : ext.id))}
          >
            <div>
              <div style={{ fontWeight: 600 }}>{ext.name}</div>
              <div style={{ color: "var(--muted-foreground)" }}>{ext.description}</div>
              <div style={{ color: "var(--muted-foreground)", fontSize: 11 }}>
                {ext.contentScripts.length} content script{ext.contentScripts.length === 1 ? "" : "s"}
              </div>
            </div>
            <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
              <label
                style={{ display: "flex", alignItems: "center", gap: 4, cursor: "pointer" }}
                onClick={(e) => e.stopPropagation()}
              >
                <input
                  type="checkbox"
                  checked={ext.enabled}
                  onChange={() => onToggle(ext.id)}
                />
                <span style={{ fontSize: 11 }}>{ext.enabled ? "On" : "Off"}</span>
              </label>
              <button
                onClick={(e) => {
                  e.stopPropagation();
                  onRemove(ext.id);
                }}
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
          {expanded === ext.id && (
            <div style={{ marginTop: 8, paddingLeft: 12, borderLeft: "2px solid var(--border)" }}>
              {ext.contentScripts.map((script) => (
                <div
                  key={script.id}
                  style={{
                    padding: "4px 0",
                    fontSize: 11,
                    color: "var(--muted-foreground)",
                  }}
                >
                  <div style={{ fontWeight: 600 }}>{script.id}</div>
                  <div>Matches: {script.matches.join(", ")}</div>
                  {script.js.length > 0 && <div>JS: {script.js.length} file(s)</div>}
                  {script.css.length > 0 && <div>CSS: {script.css.length} file(s)</div>}
                </div>
              ))}
            </div>
          )}
        </div>
      ))}
      <ExtensionInstaller onInstall={onInstall} />
    </div>
  );
}
