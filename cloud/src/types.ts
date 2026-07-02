export interface Env {
  SESSION_DO: DurableObjectNamespace;
  STAGING: R2Bucket;
  ALLOY_DATA_URL: string;
  INACTIVITY_MS: string;
  /** Set in tests / wrangler dev to skip the final mesh PUT. */
  DRY_RUN?: string;
}

/** Parsed from X-Alloy-* request headers by the Worker entry. */
export interface ChunkMeta {
  device: string;
  session: string;
  channel: string;
  seq: number;
  meshPath: string;
}

/** The device's meta.json sidecar (AlloyLogger::buildMetaJson shape). */
export interface DeviceMeta {
  device?: string;
  firmware?: string;
  session?: string; // ISO
  fields?: MetaField[];
}

export interface MetaField {
  channel: string;
  name: string;
  unit?: string;
  min?: number;
  max?: number;
  about?: string;
}

/** Response of POST {ALLOY_DATA_URL}/mesh/storage/upload-session. */
export interface UploadSession {
  bucket: string;
  endpoint_url: string;
  region: string;
  prefix: string;
  credentials: {
    access_key_id: string;
    secret_access_key: string;
    session_token: string;
  };
}
