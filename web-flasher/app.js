const VARIANTS = {
  cyd: {
    label: "CYD (ESP32)",
    manifest: "manifest-cyd.json",
    prebuiltManifests: {
      default: "manifest-cyd.json",
      "touch-off": "manifest-cyd-touch-off.json",
      "image-off": "manifest-cyd-image-off.json",
      "touch-image-off": "manifest-cyd-touch-image-off.json"
    },
    note: "CYD uses Classic Bluetooth A2DP sink on ESP32.",
    chipFamily: "ESP32"
  },
  s3: {
    label: "S3 (ESP32-S3)",
    manifest: "manifest-s3.json",
    note: "S3 uses Wi-Fi SlimProto player mode on ESP32-S3.",
    chipFamily: "ESP32-S3"
  }
};

const CYD_BT_SEED_PREFIX = "__WF_BT_NAME_SEED__:";
const CYD_BT_SEED_LEN = 31;

const installButton = document.getElementById("installButton");
const cydPrebuiltOptions = document.getElementById("cydPrebuiltOptions");
const firmwareStatus = document.getElementById("firmwareStatus");
const variantNote = document.getElementById("variant-note");
const deviceName = document.getElementById("deviceName");

let firmwareCheckToken = 0;
let customManifestUrl = null;

function getVariant() {
  const selected = document.querySelector('input[name="variant"]:checked');
  return selected ? selected.value : "cyd";
}

function getCydPrebuiltSelection() {
  const selected = document.querySelector('input[name="cydPrebuilt"]:checked');
  return selected ? selected.value : "default";
}

function getPrebuiltManifestForVariant() {
  const variant = getVariant();
  const conf = VARIANTS[variant];
  if (variant !== "cyd") {
    return conf.manifest;
  }
  const selection = getCydPrebuiltSelection();
  return conf.prebuiltManifests[selection] || conf.manifest;
}

function getSeededBtName() {
  if (getVariant() !== "cyd") {
    return null;
  }
  const value = (deviceName.value || "").trim();
  if (!value) {
    return null;
  }
  return value.slice(0, CYD_BT_SEED_LEN);
}

async function patchCydAppBytesWithBtNameSeed(sourceBytes, btName) {
  const prefixBytes = new TextEncoder().encode(CYD_BT_SEED_PREFIX);

  let markerOffset = -1;
  for (let i = 0; i <= sourceBytes.length - prefixBytes.length; i += 1) {
    let matched = true;
    for (let j = 0; j < prefixBytes.length; j += 1) {
      if (sourceBytes[i + j] !== prefixBytes[j]) {
        matched = false;
        break;
      }
    }
    if (matched) {
      markerOffset = i + prefixBytes.length;
      break;
    }
  }

  if (markerOffset < 0 || markerOffset + CYD_BT_SEED_LEN > sourceBytes.length) {
    throw new Error("CYD app binary is missing BT name seed marker");
  }

  const paddedName = btName.padEnd(CYD_BT_SEED_LEN, " ").slice(0, CYD_BT_SEED_LEN);
  const nameBytes = new TextEncoder().encode(paddedName);
  sourceBytes.set(nameBytes, markerOffset);

  // ESP images in this project use both checksum and appended hash validation.
  await updateEspImageIntegrity(sourceBytes);

  return sourceBytes;
}

function getEspImageLayout(imageBytes) {
  if (!(imageBytes instanceof Uint8Array) || imageBytes.length < 24) {
    throw new Error("Invalid ESP image: too small");
  }

  const IMAGE_HEADER_MAGIC = 0xe9;
  const IMAGE_HEADER_SIZE = 24;
  const SEGMENT_HEADER_SIZE = 8;
  const CHECKSUM_INITIAL = 0xef;

  if (imageBytes[0] !== IMAGE_HEADER_MAGIC) {
    throw new Error("Invalid ESP image header magic");
  }

  const segmentCount = imageBytes[1];
  const hashAppended = imageBytes[23] === 1;
  let offset = IMAGE_HEADER_SIZE;
  let checksum = CHECKSUM_INITIAL;

  for (let segmentIndex = 0; segmentIndex < segmentCount; segmentIndex += 1) {
    if (offset + SEGMENT_HEADER_SIZE > imageBytes.length) {
      throw new Error("Invalid ESP image: truncated segment header");
    }

    const view = new DataView(imageBytes.buffer, imageBytes.byteOffset + offset, SEGMENT_HEADER_SIZE);
    const dataLen = view.getUint32(4, true);
    offset += SEGMENT_HEADER_SIZE;

    if (offset + dataLen > imageBytes.length) {
      throw new Error("Invalid ESP image: truncated segment data");
    }

    for (let i = 0; i < dataLen; i += 1) {
      checksum ^= imageBytes[offset + i];
    }

    offset += dataLen;
  }

  const unpaddedLength = offset;
  const checksumAreaLength = ((unpaddedLength + 1 + 15) & ~15) - unpaddedLength;
  const checksumByteIndex = unpaddedLength + checksumAreaLength - 1;

  if (checksumByteIndex >= imageBytes.length) {
    throw new Error("Invalid ESP image: checksum byte out of range");
  }

  return {
    checksum,
    checksumByteIndex,
    unpaddedLength,
    checksumAreaLength,
    hashAppended,
    hashOffset: unpaddedLength + checksumAreaLength
  };
}

async function updateEspImageIntegrity(imageBytes) {
  const layout = getEspImageLayout(imageBytes);

  imageBytes[layout.checksumByteIndex] = layout.checksum;

  if (!layout.hashAppended) {
    return;
  }

  const HASH_LEN = 32;
  if (layout.hashOffset + HASH_LEN > imageBytes.length) {
    throw new Error("Invalid ESP image: hash area out of range");
  }

  if (!globalThis.crypto || !globalThis.crypto.subtle) {
    throw new Error("WebCrypto SHA-256 support is unavailable in this browser");
  }

  const digest = new Uint8Array(
    await globalThis.crypto.subtle.digest("SHA-256", imageBytes.subarray(0, layout.hashOffset))
  );

  imageBytes.set(digest, layout.hashOffset);
}

async function checkFirmwareAvailability(manifestPath) {
  const checkToken = ++firmwareCheckToken;
  firmwareStatus.textContent = "Checking firmware files...";

  try {
    const manifestUrl = new URL(manifestPath, window.location.href).href;
    const manifestResponse = await fetch(manifestUrl, { cache: "no-cache" });
    if (!manifestResponse.ok) {
      throw new Error(`Manifest not found (${manifestResponse.status})`);
    }

    const manifest = await manifestResponse.json();
    const firstBuild = Array.isArray(manifest.builds) ? manifest.builds[0] : null;
    if (!firstBuild || !Array.isArray(firstBuild.parts) || firstBuild.parts.length === 0) {
      throw new Error("Manifest has no flash parts");
    }

    const missing = [];
    for (const part of firstBuild.parts) {
      const partUrl = new URL(part.path, manifestUrl).href;
      const response = await fetch(partUrl, { method: "HEAD", cache: "no-cache" });
      if (!response.ok) {
        missing.push(`${part.path} (${response.status})`);
      }
    }

    if (checkToken !== firmwareCheckToken) {
      return;
    }

    firmwareStatus.textContent = missing.length
      ? `Missing firmware files: ${missing.join(", ")}`
      : "Firmware files are available for this variant.";
  } catch (error) {
    if (checkToken !== firmwareCheckToken) {
      return;
    }
    firmwareStatus.textContent = `Firmware check failed: ${error.message}`;
  }
}

function absolutizeManifestParts(manifest, manifestUrl) {
  if (!manifest || !Array.isArray(manifest.builds)) {
    return manifest;
  }

  return {
    ...manifest,
    builds: manifest.builds.map((build) => {
      if (!build || !Array.isArray(build.parts)) {
        return build;
      }

      return {
        ...build,
        parts: build.parts.map((part) => ({
          ...part,
          path: new URL(part.path, manifestUrl).href
        }))
      };
    })
  };
}

async function createInstallManifest() {
  const variant = getVariant();
  const manifestPath = getPrebuiltManifestForVariant();
  const manifestUrl = new URL(manifestPath, window.location.href).href;
  const response = await fetch(manifestUrl, { cache: "no-cache" });
  if (!response.ok) {
    throw new Error(`Failed loading manifest (${response.status})`);
  }

  const manifest = absolutizeManifestParts(await response.json(), manifestUrl);

  if (variant !== "cyd") {
    return manifest;
  }

  const btName = getSeededBtName();
  if (!btName) {
    return manifest;
  }

  const build = manifest.builds && manifest.builds[0];
  if (!build || !Array.isArray(build.parts)) {
    throw new Error("Manifest has invalid build parts");
  }

  const appPart = build.parts.find((part) => Number(part.offset) === 0x10000);
  if (!appPart) {
    throw new Error("Manifest missing app part at 0x10000");
  }

  const appUrl = new URL(appPart.path, manifestUrl).href;
  const appResponse = await fetch(appUrl, { cache: "no-cache" });
  if (!appResponse.ok) {
    throw new Error(`Failed downloading app binary (${appResponse.status})`);
  }

  const sourceBytes = new Uint8Array(await appResponse.arrayBuffer());
  const patchedBytes = await patchCydAppBytesWithBtNameSeed(sourceBytes, btName);
  const patchedFile = new File([patchedBytes], "bluetooth_music_player_cyd_patched.bin", { type: "application/octet-stream" });
  const patchedBlobUrl = URL.createObjectURL(patchedFile);

  const nextManifest = {
    ...manifest,
    name: `${manifest.name || "CYD Music Player"} (${btName})`,
    builds: manifest.builds.map((buildItem, index) => {
      if (index !== 0) {
        return buildItem;
      }
      return {
        ...buildItem,
        parts: buildItem.parts.map((part) => {
          if (Number(part.offset) !== 0x10000) {
            return part;
          }
          return {
            ...part,
            path: patchedBlobUrl
          };
        })
      };
    })
  };

  return nextManifest;
}

async function refreshInstallManifest() {
  try {
    const manifest = await createInstallManifest();
    if (customManifestUrl) {
      URL.revokeObjectURL(customManifestUrl);
      customManifestUrl = null;
    }
    const blob = new Blob([JSON.stringify(manifest, null, 2)], { type: "application/json" });
    customManifestUrl = URL.createObjectURL(blob);
    installButton.setAttribute("manifest", customManifestUrl);
  } catch (error) {
    firmwareStatus.textContent = `Manifest setup failed: ${error.message}`;
  }
}

async function updateVariantUI() {
  const variant = getVariant();
  const conf = VARIANTS[variant];
  variantNote.textContent = conf.note;

  if (cydPrebuiltOptions) {
    cydPrebuiltOptions.classList.toggle("hidden", variant !== "cyd");
  }

  const cydOnly = document.querySelectorAll(".cyd-only");
  for (const el of cydOnly) {
    const hidden = variant !== "cyd";
    el.classList.toggle("hidden", hidden);
    el.querySelectorAll("input, select, textarea, button").forEach((field) => {
      field.disabled = hidden;
    });
  }

  if (variant === "cyd" && !deviceName.value) {
    deviceName.value = "CYD Music Player";
  }

  await refreshInstallManifest();
  await checkFirmwareAvailability(getPrebuiltManifestForVariant());
}

document.querySelectorAll('input[name="variant"]').forEach((r) => {
  r.addEventListener("change", async () => {
    if (getVariant() !== "cyd") {
      deviceName.value = "";
    }
    await updateVariantUI();
  });
});

document.querySelectorAll('input[name="cydPrebuilt"]').forEach((r) => {
  r.addEventListener("change", async () => {
    if (getVariant() === "cyd") {
      await refreshInstallManifest();
      await checkFirmwareAvailability(getPrebuiltManifestForVariant());
    }
  });
});

deviceName.addEventListener("input", async () => {
  if (getVariant() === "cyd") {
    await refreshInstallManifest();
  }
});

updateVariantUI();
