export interface Extension {
  id: string;
  name: string;
  description: string;
  enabled: boolean;
  version: string;
}

export interface TabGroup {
  id: string;
  name: string;
  color: string;
}
