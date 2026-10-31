import type { DownloadItem } from "../downloads";

export function DownloadsPanel({
  downloads,
  onClear,
}: {
  downloads: DownloadItem[];
  onClear: () => void;
}) {
  return (
    <div style={{ padding: 12 }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 8 }}>
        <h3 style={{ margin: 0 }}>Downloads</h3>
        <div style={{ display: "flex", gap: 8 }}>
          <button
            onClick={() => onClear()}
            style={{
              padding: "4px 8px",
              background: "transparent",
              border: "1px solid var(--border)",
              color: "var(--foreground)",
              cursor: "pointer",
              borderRadius: 4,
              fontSize: 12,
            }}
          >
            Clear completed
          </button>
        </div>
      </div>
      {downloads.length === 0 && (
        <p style={{ fontSize: 12, color: "var(--muted-foreground)" }}>No downloads yet.</p>
      )}
      {downloads.map((item) => (
        <div
          key={item.id}
          style={{
            padding: "8px 6px",
            borderBottom: "1px solid var(--border)",
            fontSize: 12,
          }}
        >
          <div style={{ fontWeight: 600 }}>{item.filename}</div>
          <div style={{ color: "var(--muted-foreground)", marginTop: 2 }}>{item.status}</div>
          {item.status === "downloading" && (
            <div
              style={{
                height: 4,
                background: "var(--border)",
                borderRadius: 2,
                marginTop: 4,
                overflow: "hidden",
              }}
            >
              <div
                style={{
                  height: "100%",
                  width: `${item.progress}%`,
                  background: "var(--accent)",
                  borderRadius: 2,
                }}
              />
            </div>
          )}
          {item.size !== null && (
            <div style={{ color: "var(--muted-foreground)" }}>
              {(item.size / 1024).toFixed(1)} KB
            </div>
          )}
        </div>
      ))}
    </div>
  );
}
