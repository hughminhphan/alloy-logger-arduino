import { describe, expect, it } from "vitest";
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { McapIndexedReader } from "@mcap/core";
import type { IReadable } from "@mcap/core";
import { assembleMcap, buildSchema } from "../src/mcap";
import type { ChunkSource } from "../src/csv";
import type { DeviceMeta } from "../src/types";

const FIXTURES = join(import.meta.dirname, "fixtures", "robots-sbr-1782961596");

class BufferReadable implements IReadable {
  constructor(private buf: Uint8Array) {}
  async size(): Promise<bigint> {
    return BigInt(this.buf.byteLength);
  }
  async read(offset: bigint, size: bigint): Promise<Uint8Array> {
    return this.buf.subarray(Number(offset), Number(offset) + Number(size));
  }
}

function sourceFromBuffers(channel: string, bodies: Uint8Array[]): ChunkSource {
  return {
    channel,
    chunks: async function* () {
      for (const b of bodies) yield b;
    },
  };
}

function fixtureSource(channel: string): { src: ChunkSource; rowCount: number } {
  const files = readdirSync(FIXTURES)
    .filter((f) => f.startsWith(`sbr-01_${channel}_`) && f.endsWith(".csv"))
    .sort((a, b) => {
      const seq = (f: string) => Number(f.match(/_(\d+)\.csv$/)![1]);
      return seq(a) - seq(b);
    });
  expect(files.length).toBeGreaterThan(0);
  const bodies = files.map((f) => new Uint8Array(readFileSync(join(FIXTURES, f))));
  let rowCount = 0;
  for (const b of bodies) {
    const lines = new TextDecoder().decode(b).split("\n");
    rowCount += lines.filter((l, i) => i > 0 && l.length > 0).length;
  }
  return { src: sourceFromBuffers(channel, bodies), rowCount };
}

const enc = new TextEncoder();
const csv = (s: string) => enc.encode(s);

describe("assembleMcap on the real SBR session", () => {
  it("produces an indexed MCAP with every row, time-ordered, schema carrying units", async () => {
    const meta = JSON.parse(
      readFileSync(join(FIXTURES, "sbr-01_meta.json"), "utf8"),
    ) as DeviceMeta;
    const { src, rowCount } = fixtureSource("balance");

    const bytes = (await assembleMcap([src], meta, {
      device: "sbr-01",
      session: "1782961596",
      meshPath: "robots/sbr",
    }))!;
    expect(bytes.byteLength).toBeGreaterThan(1000);
    // keep the artifact around for `mcap doctor` / Foxglove inspection
    writeFileSync(join(import.meta.dirname, "out-sbr.mcap"), bytes);

    const reader = await McapIndexedReader.Initialize({
      readable: new BufferReadable(bytes),
    });
    expect(reader.chunkIndexes.length).toBeGreaterThan(0);
    expect(reader.channelsById.size).toBe(1);
    expect(reader.statistics?.messageCount).toBe(BigInt(rowCount));

    const chan = [...reader.channelsById.values()][0];
    expect(chan.topic).toBe("/balance");
    expect(chan.messageEncoding).toBe("json");
    const schema = reader.schemasById.get(chan.schemaId)!;
    expect(schema.encoding).toBe("jsonschema");
    const schemaJson = JSON.parse(new TextDecoder().decode(schema.data));
    expect(schemaJson.properties.pitch.description).toContain("deg");
    expect(schemaJson.properties.pitch.minimum).toBe(-90);

    // messages stream back in log-time order with intact payloads
    let prev = 0n;
    let n = 0;
    let firstPayload: Record<string, number | null> | null = null;
    for await (const msg of reader.readMessages()) {
      expect(msg.logTime >= prev).toBe(true);
      prev = msg.logTime;
      if (n === 0) {
        firstPayload = JSON.parse(new TextDecoder().decode(msg.data));
      }
      n++;
    }
    expect(n).toBe(rowCount);
    expect(firstPayload).toMatchObject({ pitch: -0.5625, setpoint: 0.5 });
    expect(firstPayload).not.toHaveProperty("t_ns");
  });
});

describe("assembleMcap edge cases", () => {
  it("k-way merges interleaved channels into global time order", async () => {
    const a = sourceFromBuffers("alpha", [
      csv("t_ns,x\n0000000000000000100,1\n0000000000000000300,3\n"),
    ]);
    const b = sourceFromBuffers("beta", [
      csv("t_ns,y\n0000000000000000200,2\n0000000000000000400,4\n"),
    ]);
    const bytes = (await assembleMcap([a, b], null, {
      device: "d",
      session: "1",
      meshPath: "m",
    }))!;
    const reader = await McapIndexedReader.Initialize({
      readable: new BufferReadable(bytes),
    });
    const times: bigint[] = [];
    for await (const msg of reader.readMessages()) times.push(msg.logTime);
    expect(times).toEqual([100n, 200n, 300n, 400n]);
    expect(reader.channelsById.size).toBe(2);
  });

  it("starts a new schema generation when a channel's field set changes", async () => {
    const src = sourceFromBuffers("io", [
      csv("t_ns,btn\n0000000000000000100,1\n"),
      csv("t_ns,btn,led\n0000000000000000200,0,1\n"),
    ]);
    const bytes = (await assembleMcap([src], null, {
      device: "d",
      session: "1",
      meshPath: "m",
    }))!;
    const reader = await McapIndexedReader.Initialize({
      readable: new BufferReadable(bytes),
    });
    expect(reader.channelsById.size).toBe(2);
    expect(reader.schemasById.size).toBe(2);
    const payloads: unknown[] = [];
    for await (const msg of reader.readMessages()) {
      payloads.push(JSON.parse(new TextDecoder().decode(msg.data)));
    }
    expect(payloads).toEqual([{ btn: 1 }, { btn: 0, led: 1 }]);
  });

  it("maps empty cells (device NaN encoding) to null", async () => {
    const src = sourceFromBuffers("adc", [
      csv("t_ns,v\n0000000000000000100,\n0000000000000000200,3.5\n"),
    ]);
    const bytes = (await assembleMcap([src], null, {
      device: "d",
      session: "1",
      meshPath: "m",
    }))!;
    const reader = await McapIndexedReader.Initialize({
      readable: new BufferReadable(bytes),
    });
    const payloads: unknown[] = [];
    for await (const msg of reader.readMessages()) {
      payloads.push(JSON.parse(new TextDecoder().decode(msg.data)));
    }
    expect(payloads).toEqual([{ v: null }, { v: 3.5 }]);
  });
});

describe("buildSchema", () => {
  it("folds meta.json semantics into property descriptions", () => {
    const meta: DeviceMeta = {
      fields: [
        { channel: "balance", name: "pitch", unit: "deg", min: -90, max: 90, about: "tilt" },
      ],
    };
    const s = buildSchema("balance", ["pitch", "raw"], meta) as {
      properties: Record<string, { description?: string; minimum?: number }>;
    };
    expect(s.properties.pitch.description).toBe("deg; tilt; range -90..90");
    expect(s.properties.pitch.minimum).toBe(-90);
    expect(s.properties.raw.description).toBeUndefined();
  });
});
