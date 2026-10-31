import { useState } from "react";
import type { Extension, ContentScript } from "../extensions";

export function ExtensionInstaller({ onInstall }: { onInstall: (ext: Extension) => void }) {
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [matches, setMatches] = useState("<all_urls>");
  const [js, setJs] = useState("");
  const [css, setCss] = useState("");

  const handleSubmit = () => {
    if (!name.trim()) return;
    const script: ContentScript = {
      id: crypto.randomUUID(),
      matches: matches.split(",").map((m) => m.trim()).filter(Boolean),
      js: js.split("\n").map((s) => s.trim()).filter(Boolean),
      css: css.split("\n").map((s) => s.trim()).filter(Boolean),
    };
    onInstall({
      id: crypto.randomUUID(),
      name: name.trim(),
      description: description.trim() || name.trim(),
      enabled: true,
      version: "1.0.0",
      contentScripts: [script],
    });
    setName("");
    setDescription("");
    setMatches("<all_urls>");
    setJs("");
    setCss("");
  };

  return (
    <div style={{ padding: 12 }}>
      <h3 style={{ margin: "0 0 12px" }}>Install Extension</h3>
      <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
        <input
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder="Extension name"
          style={{
            padding: "4px 6px",
            background: "var(--border)",
            color: "var(--foreground)",
            border: "1px solid var(--border)",
            borderRadius: 4,
            outline: "none",
            fontSize: 12,
          }}
        />
        <input
          value={description}
          onChange={(e) => setDescription(e.target.value)}
          placeholder="Description"
          style={{
            padding: "4px 6px",
            background: "var(--border)",
            color: "var(--foreground)",
            border: "1px solid var(--border)",
            borderRadius: 4,
            outline: "none",
            fontSize: 12,
          }}
        />
        <input
          value={matches}
          onChange={(e) => setMatches(e.target.value)}
          placeholder="URL matches (comma separated)"
          style={{
            padding: "4px 6px",
            background: "var(--border)",
            color: "var(--foreground)",
            border: "1px solid var(--border)",
            borderRadius: 4,
            outline: "none",
            fontSize: 12,
          }}
        />
        <textarea
          value={js}
          onChange={(e) => setJs(e.target.value)}
          placeholder="JavaScript (one per line)"
          rows={3}
          style={{
            padding: "4px 6px",
            background: "var(--border)",
            color: "var(--foreground)",
            border: "1px solid var(--border)",
            borderRadius: 4,
            outline: "none",
            fontSize: 12,
            fontFamily: "monospace",
          }}
        />
        <textarea
          value={css}
          onChange={(e) => setCss(e.target.value)}
          placeholder="CSS (one per line)"
          rows={2}
          style={{
            padding: "4px 6px",
            background: "var(--border)",
            color: "var(--foreground)",
            border: "1px solid var(--border)",
            borderRadius: 4,
            outline: "none",
            fontSize: 12,
            fontFamily: "monospace",
          }}
        />
        <button
          onClick={handleSubmit}
          style={{
            padding: "6px 12px",
            background: "var(--accent)",
            border: "none",
            color: "#fff",
            cursor: "pointer",
            borderRadius: 4,
            fontSize: 12,
          }}
        >
          Install
        </button>
      </div>
    </div>
  );
}
