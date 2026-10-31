export interface Extension {
  id: string;
  name: string;
  description: string;
  enabled: boolean;
  version: string;
  contentScripts: ContentScript[];
}

export interface ContentScript {
  id: string;
  matches: string[];
  js: string[];
  css: string[];
}

export interface TabGroup {
  id: string;
  name: string;
  color: string;
}
