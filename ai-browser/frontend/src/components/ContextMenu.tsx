import { useEffect } from "react";

export function ContextMenu({ x, y, onClose, children }: { x: number; y: number; onClose: () => void; children: React.ReactNode }) {
  useEffect(() => {
    const handler = () => onClose();
    window.addEventListener("click", handler);
    return () => window.removeEventListener("click", handler);
  }, [onClose]);

  return (
    <div
      onClick={(e) => e.stopPropagation()}
      style={{
        position: "fixed",
        left: x,
        top: y,
        background: "var(--card)",
        border: "1px solid var(--border)",
        borderRadius: 6,
        padding: 4,
        minWidth: 160,
        zIndex: 2000,
        boxShadow: "0 4px 12px rgba(0,0,0,0.3)",
      }}
    >
      {children}
    </div>
  );
}

export function ContextMenuItem({ onClick, children, disabled }: { onClick: () => void; children: React.ReactNode; disabled?: boolean }) {
  return (
    <div
      onClick={() => {
        if (!disabled) {
          onClick();
        }
      }}
      style={{
        padding: "6px 12px",
        fontSize: 12,
        cursor: disabled ? "default" : "pointer",
        borderRadius: 4,
        opacity: disabled ? 0.5 : 1,
        color: "var(--foreground)",
      }}
    >
      {children}
    </div>
  );
}
