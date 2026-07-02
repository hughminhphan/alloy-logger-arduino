// Alloy data-API client: mint upload-sessions and SigV4-PUT files into a user's mesh.
// Same protocol the device speaks in direct mode (see src/AlloyUploader.h), verified live.

import { AwsClient } from "aws4fetch";
import type { UploadSession } from "./types";

export async function mintUploadSession(
  dataUrl: string,
  apiKey: string,
  path: string,
): Promise<UploadSession | null> {
  const res = await fetch(`${dataUrl}/mesh/storage/upload-session`, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${apiKey}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ path, ttl_seconds: 900 }),
  });
  if (!res.ok) return null;
  const sess = (await res.json()) as UploadSession;
  if (!sess.bucket || !sess.endpoint_url || !sess.credentials?.access_key_id) return null;
  return sess;
}

export async function putToMesh(
  sess: UploadSession,
  filename: string,
  body: Uint8Array,
  contentType: string,
): Promise<boolean> {
  const aws = new AwsClient({
    accessKeyId: sess.credentials.access_key_id,
    secretAccessKey: sess.credentials.secret_access_key,
    sessionToken: sess.credentials.session_token,
    service: "s3",
    region: sess.region,
    retries: 0, // the DO's alarm backoff is the retry layer — keep failures visible to it
  });
  const endpoint = sess.endpoint_url.replace(/\/$/, "");
  const prefix = sess.prefix.endsWith("/") ? sess.prefix : `${sess.prefix}/`;
  const res = await aws.fetch(`${endpoint}/${sess.bucket}/${prefix}${filename}`, {
    method: "PUT",
    headers: {
      "x-amz-content-sha256": "UNSIGNED-PAYLOAD",
      "Content-Type": contentType,
    },
    body,
  });
  return res.ok;
}
