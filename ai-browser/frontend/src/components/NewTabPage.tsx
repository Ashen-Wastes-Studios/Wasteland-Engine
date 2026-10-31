import type { Bookmark } from "../shared-types";

export function NewTabPage({ bookmarks, onNavigate }: { bookmarks: Bookmark[]; onNavigate: (url: string) => void }) {
  return (
    <div
      style={{
        height: "100%",
        display: "flex",
        flexDirection: "column",
        alignItems: "center",
        justifyContent: "center",
        gap: 24,
        padding: 24,
      }}
    >
      <h1 style={{ fontSize: 48, fontWeight: 300, color: "var(--accent)" }}>AI Browser</h1>
      <input
        id="new-tab-search"
        placeholder="Search the web..."
        style={{
          width: "100%",
          maxWidth: 480,
          padding: "10px 16px",
          background: "var(--border)",
          color: "var(--foreground)",
          border: "1px solid var(--border)",
          borderRadius: 24,
          outline: "none",
          fontSize: 14,
        }}
        onKeyDown={(e) => {
          if (e.key === "Enter" && e.currentTarget.value.trim()) {
            onNavigate(e.currentTarget.value.trim());
          }
        }}
      />
      {bookmarks.length > 0 && (
        <div style={{ display: "flex", gap: 12, flexWrap: "wrap", justifyContent: "center" }}>
          {bookmarks.slice(0, 12).map((bookmark, i) => (
            <button
              key={`${bookmark.url}-${i}`}
              onClick={() => onNavigate(bookmark.url)}
              style={{
                padding: "8px 16px",
                background: "var(--border)",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 20,
                fontSize: 13,
              }}
            >
              {bookmark.title || bookmark.url}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
