import { useState, useEffect } from "react";

export function FindBar({ onClose }: { onClose: () => void }) {
  const [query, setQuery] = useState("");

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key === "f") {
        e.preventDefault();
      }
      if (e.key === "Escape") {
        onClose();
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [onClose]);

  return (
    <div
      style={{
        padding: 8,
        borderBottom: "1px solid var(--border)",
        display: "flex",
        gap: 8,
        alignItems: "center",
      }}
    >
      <input
        value={query}
        onChange={(e) => setQuery(e.target.value)}
        placeholder="Find in page"
        autoFocus
        style={{
          flex: 1,
          padding: "4px 8px",
          background: "var(--border)",
          color: "var(--foreground)",
          border: "1px solid var(--border)",
          borderRadius: 4,
          outline: "none",
        }}
      />
      <button
        onClick={onClose}
        style={{
          padding: "4px 8px",
          background: "transparent",
          border: "1px solid var(--border)",
          color: "var(--foreground)",
          cursor: "pointer",
          borderRadius: 4,
        }}
      >
        ✕
      </button>
    </div>
  );
}
