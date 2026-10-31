export function TabBar({
  tabs,
  active,
  onSelect,
  onClose,
  onNewTab,
}: {
  tabs: { id: string; title: string }[];
  active: string;
  onSelect: (id: string) => void;
  onClose: (id: string) => void;
  onNewTab: () => void;
}) {
  return (
    <div
      style={{
        display: "flex",
        alignItems: "center",
        background: "var(--card)",
        borderBottom: "1px solid var(--border)",
        height: 36,
      }}
    >
      {tabs.map((tab) => (
        <div
          key={tab.id}
          onClick={() => onSelect(tab.id)}
          style={{
            padding: "6px 12px",
            cursor: "pointer",
            background: tab.id === active ? "var(--border)" : "transparent",
            borderRight: "1px solid var(--border)",
            display: "flex",
            alignItems: "center",
            gap: 8,
            fontSize: 12,
          }}
        >
          <span>{tab.title}</span>
          <span
            onClick={(e) => {
              e.stopPropagation();
              onClose(tab.id);
            }}
            style={{ opacity: 0.6, cursor: "pointer", fontSize: 10 }}
          >
            ✕
          </span>
        </div>
      ))}
      <button
        onClick={onNewTab}
        style={{
          marginLeft: 8,
          marginRight: 8,
          background: "transparent",
          border: "1px solid var(--border)",
          color: "var(--foreground)",
          cursor: "pointer",
          borderRadius: 4,
          width: 24,
          height: 24,
        }}
      >
        +
      </button>
    </div>
  );
}
