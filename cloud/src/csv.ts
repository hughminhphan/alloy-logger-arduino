// CSV chunk parsing + per-channel row cursors.
//
// A chunk is what the device seals: one header line "t_ns,<field>,<field>,..." followed by bare
// value rows. Chunks of one channel are seq-ordered and time-ordered; the field set only changes
// at a chunk boundary (the device reseals on field-set change), which we surface as a new
// "generation" so the assembler can register a fresh MCAP schema/channel pair.

export interface ChunkSource {
  channel: string;
  /** Seq-ordered chunk bodies. */
  chunks: () => AsyncGenerator<Uint8Array>;
}

export interface CursorRow {
  channel: string;
  /** Bumps when the header (field set) changes mid-stream. */
  generation: number;
  /** Field names, t_ns excluded. Shared (same array identity) across a generation's rows. */
  fields: string[];
  tNs: bigint;
  /** Aligned with fields; empty CSV cell (the device's NaN encoding) becomes null. */
  values: (number | null)[];
}

const dec = new TextDecoder();

/** "t_ns,pitch,setpoint" -> ["pitch", "setpoint"]; throws on a malformed header. */
export function parseHeader(line: string): string[] {
  const cells = line.split(",");
  if (cells[0] !== "t_ns" || cells.length < 2) {
    throw new Error(`bad CSV header: ${JSON.stringify(line.slice(0, 80))}`);
  }
  return cells.slice(1);
}

/** Stream every row of a channel across its chunks, tracking header generations. */
export async function* rowsOf(src: ChunkSource): AsyncGenerator<CursorRow> {
  let fields: string[] | null = null;
  let generation = -1;
  let headerLine: string | null = null;

  for await (const chunk of src.chunks()) {
    const lines = dec.decode(chunk).split("\n");
    if (lines.length === 0 || lines[0] === "") continue;
    if (lines[0] !== headerLine) {
      headerLine = lines[0];
      fields = parseHeader(headerLine);
      generation++;
    }
    for (let i = 1; i < lines.length; i++) {
      const line = lines[i];
      if (!line) continue; // trailing newline / blank
      const cells = line.split(",");
      let tNs: bigint;
      try {
        tNs = BigInt(cells[0]);
      } catch {
        continue; // malformed stamp — skip the row rather than poison the session
      }
      const values: (number | null)[] = new Array(fields!.length);
      for (let j = 0; j < fields!.length; j++) {
        const c = cells[j + 1];
        if (c === undefined || c === "") {
          values[j] = null;
        } else {
          const n = Number(c);
          values[j] = Number.isNaN(n) ? null : n;
        }
      }
      yield { channel: src.channel, generation, fields: fields!, tNs, values };
    }
  }
}
