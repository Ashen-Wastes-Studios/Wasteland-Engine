import { useState, useCallback } from "react";
import type { PasswordEntry } from "../passwords";

const PASSWORDS_KEY = "ai-browser-passwords";

function loadPasswords(): PasswordEntry[] {
  try {
    const raw = localStorage.getItem(PASSWORDS_KEY);
    return raw ? (JSON.parse(raw) as PasswordEntry[]) : [];
  } catch {
    return [];
  }
}

export function usePasswords() {
  const [passwords, setPasswords] = useState<PasswordEntry[]>(loadPasswords);

  const persist = useCallback((items: PasswordEntry[]) => {
    localStorage.setItem(PASSWORDS_KEY, JSON.stringify(items));
  }, []);

  const addPassword = useCallback(
    (entry: PasswordEntry) => {
      setPasswords((prev) => {
        const next = [...prev, entry];
        persist(next);
        return next;
      });
    },
    [persist],
  );

  const removePassword = useCallback(
    (id: string) => {
      setPasswords((prev) => {
        const next = prev.filter((item) => item.id !== id);
        persist(next);
        return next;
      });
    },
    [persist],
  );

  return { passwords, addPassword, removePassword };
}
