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
    chipFamily: "ESP32",
    defaultParts: {
      bootloader: "firmware/cyd/bootloader.bin",
      partition: "firmware/cyd/partition-table.bin",
      app: "firmware/cyd/bluetooth_music_player_cyd.bin"
    },
    note: "CYD uses Classic Bluetooth A2DP sink on ESP32.",
    sedFile: "CYD-BT/main.h",
    deviceKey: "BT_DEVICE_NAME",
    buildCmd: [
      "idf.py set-target esp32",
      "idf.py build",
      "idf.py -p /dev/ttyUSB0 flash monitor"
    ]
  },
  s3: {
    label: "S3 (ESP32-S3)",
    manifest: "manifest-s3.json",
    chipFamily: "ESP32-S3",
    defaultParts: {
      bootloader: "firmware/s3/bootloader.bin",
      partition: "firmware/s3/partition-table.bin",
      app: "firmware/s3/bluetooth_music_player_s3.bin"
    },
    note: "S3 uses Wi-Fi SlimProto player mode on ESP32-S3.",
    sedFile: "S3-BT/main/main.h",
    deviceKey: "BT_DEVICE_NAME",
    buildCmd: [
      "idf.py -C S3-BT set-target esp32s3",
      "idf.py -C S3-BT build",
      "idf.py -C S3-BT -p /dev/ttyUSB0 flash monitor"
    ]
  }
};

const CYD_BT_SEED_PREFIX = "__WF_BT_NAME_SEED__:";
const CYD_BT_SEED_LEN = 31;

const installButton = document.getElementById("installButton");
const cydPrebuiltOptions = document.getElementById("cydPrebuiltOptions");
const customInstallButton = document.getElementById("customInstallButton");
const firmwareStatus = document.getElementById("firmwareStatus");
const customScriptStatus = document.getElementById("customScriptStatus");

const bootloaderUrl = document.getElementById("bootloaderUrl");
const partitionUrl = document.getElementById("partitionUrl");
const appUrl = document.getElementById("appUrl");
const bootloaderFile = document.getElementById("bootloaderFile");
const partitionFile = document.getElementById("partitionFile");
const appFile = document.getElementById("appFile");

const applyCustomScriptBtn = document.getElementById("applyCustomScriptBtn");
const downloadManifestBtn = document.getElementById("downloadManifestBtn");

const output = document.getElementById("output");
const variantNote = document.getElementById("variant-note");
const generateBtn = document.getElementById("generateBtn");
const downloadBtn = document.getElementById("downloadBtn");

const deviceName = document.getElementById("deviceName");
const disableTouch = document.getElementById("disableTouch");
const disableImageLoading = document.getElementById("disableImageLoading");
const wifiSsid = document.getElementById("wifiSsid");
const wifiPassword = document.getElementById("wifiPassword");
const slimHost = document.getElementById("slimHost");
const slimPort = document.getElementById("slimPort");

let customManifestUrl = null;
let firmwareCheckToken = 0;
const customPartBlobUrls = [];

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

function shellEscapeSingle(value) {
  return String(value).replace(/'/g, `"'"'"`);
}

function toAbsolutePath(pathToken) {
  return new URL(pathToken, window.location.href).href;
}

function releaseCustomPartBlobUrls() {
  while (customPartBlobUrls.length) {
    const url = customPartBlobUrls.pop();
    URL.revokeObjectURL(url);
  }
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

async function patchCydAppBinaryWithBtNameSeed(file) {
  const btName = getSeededBtName();
  if (!btName) {
    return file;
  }

  const sourceBytes = new Uint8Array(await file.arrayBuffer());
  const patchedBytes = patchCydAppBytesWithBtNameSeed(sourceBytes, btName);

  return new File([patchedBytes], file.name, { type: "application/octet-stream" });
}

function patchCydAppBytesWithBtNameSeed(sourceBytes, btName) {
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

  return sourceBytes;
}

async function resolveCustomPart({ urlField, fileField, offset, label, patchCydApp }) {
  let file = fileField.files && fileField.files.length ? fileField.files[0] : null;
  if (file && patchCydApp) {
    file = await patchCydAppBinaryWithBtNameSeed(file);
  }
  if (file) {
    const blobUrl = URL.createObjectURL(file);
    customPartBlobUrls.push(blobUrl);
    return { offset, path: blobUrl };
  }

  const url = String(urlField.value || "").trim();
  if (!url) {
    throw new Error(`${label} requires either a URL or file upload.`);
  }

  const absoluteUrl = toAbsolutePath(url);
  if (patchCydApp) {
    const btName = getSeededBtName();
    if (btName) {
      const response = await fetch(absoluteUrl);
      if (!response.ok) {
        throw new Error(`Failed downloading ${label} URL (${response.status}).`);
      }
      const sourceBytes = new Uint8Array(await response.arrayBuffer());
      const patchedBytes = patchCydAppBytesWithBtNameSeed(sourceBytes, btName);
      const patchedFile = new File([patchedBytes], "app-patched.bin", { type: "application/octet-stream" });
      const blobUrl = URL.createObjectURL(patchedFile);
      customPartBlobUrls.push(blobUrl);
      return { offset, path: blobUrl };
    }
  }

  return { offset, path: absoluteUrl };
}

async function createCustomManifest() {
  const variant = getVariant();
  const chipFamily = VARIANTS[variant].chipFamily;

  releaseCustomPartBlobUrls();

  const parts = [
    await resolveCustomPart({
      urlField: bootloaderUrl,
      fileField: bootloaderFile,
      offset: 0x1000,
      label: "Bootloader"
    }),
    await resolveCustomPart({
      urlField: partitionUrl,
      fileField: partitionFile,
      offset: 0x8000,
      label: "Partition Table"
    }),
    await resolveCustomPart({
      urlField: appUrl,
      fileField: appFile,
      offset: 0x10000,
      label: "Application",
      patchCydApp: variant === "cyd"
    })
  ];

  return {
    name: `Custom Flash (${chipFamily})`,
    version: "1.0.0",
    new_install_prompt_erase: true,
    builds: [
      {
        chipFamily,
        parts
      }
    ]
  };
}

function setCustomManifest(manifestObject) {
  if (customManifestUrl) {
    URL.revokeObjectURL(customManifestUrl);
  }
  const blob = new Blob([JSON.stringify(manifestObject, null, 2)], { type: "application/json" });
  customManifestUrl = URL.createObjectURL(blob);
  customInstallButton.setAttribute("manifest", customManifestUrl);
}

async function applyCustomScript() {
  try {
    const manifestObject = await createCustomManifest();
    setCustomManifest(manifestObject);
    customScriptStatus.textContent = `Custom manifest ready: ${manifestObject.builds[0].parts.length} part(s).`;
  } catch (error) {
    customScriptStatus.textContent = `Custom source error: ${error.message}`;
  }
}

async function downloadCustomManifest() {
  try {
    const manifestObject = await createCustomManifest();
    const blob = new Blob([JSON.stringify(manifestObject, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = manifestObject.builds[0].chipFamily === "ESP32-S3" ? "manifest-custom-s3.json" : "manifest-custom-esp32.json";
    a.click();
    URL.revokeObjectURL(url);
    customScriptStatus.textContent = "Custom manifest downloaded.";
  } catch (error) {
    customScriptStatus.textContent = `Custom source error: ${error.message}`;
  }
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

    if (missing.length) {
      firmwareStatus.textContent = `Missing firmware files: ${missing.join(", ")}`;
    } else {
      firmwareStatus.textContent = "Firmware files are available for this variant.";
    }
  } catch (error) {
    if (checkToken !== firmwareCheckToken) {
      return;
    }
    firmwareStatus.textContent = `Firmware check failed: ${error.message}`;
  }
}

function seedCustomSourcesFromVariant() {
  const conf = VARIANTS[getVariant()];
  bootloaderUrl.value = conf.defaultParts.bootloader;
  partitionUrl.value = conf.defaultParts.partition;
  appUrl.value = conf.defaultParts.app;
  bootloaderFile.value = "";
  partitionFile.value = "";
  appFile.value = "";
  applyCustomScript();
}

function updateVariantUI() {
  const variant = getVariant();
  const conf = VARIANTS[variant];
  installButton.setAttribute("manifest", getPrebuiltManifestForVariant());
  variantNote.textContent = conf.note;

  if (cydPrebuiltOptions) {
    cydPrebuiltOptions.classList.toggle("hidden", variant !== "cyd");
  }

  const s3Only = document.querySelectorAll(".s3-only");
  for (const el of s3Only) {
    const hidden = variant !== "s3";
    el.classList.toggle("hidden", hidden);
    el.querySelectorAll("input, select, textarea, button").forEach((field) => {
      field.disabled = hidden;
    });
  }

  const cydOnly = document.querySelectorAll(".cyd-only");
  for (const el of cydOnly) {
    const hidden = variant !== "cyd";
    el.classList.toggle("hidden", hidden);
    el.querySelectorAll("input, select, textarea, button").forEach((field) => {
      field.disabled = hidden;
    });
  }

  if (!deviceName.value) {
    deviceName.value = variant === "s3" ? "S3 Music Player" : "CYD Music Player";
  }

  seedCustomSourcesFromVariant();
  checkFirmwareAvailability(getPrebuiltManifestForVariant());
}

function generateScript() {
  const variant = getVariant();
  const conf = VARIANTS[variant];

  const name = deviceName.value.trim() || (variant === "s3" ? "S3 Music Player" : "CYD Music Player");
  const escName = shellEscapeSingle(name);

  const lines = [];
  lines.push("#!/usr/bin/env bash");
  lines.push("set -euo pipefail");
  lines.push("");
  lines.push("# Generated by web-flasher UI");
  lines.push(`# Variant: ${conf.label}`);
  lines.push("");

  lines.push(`# 1) Apply config overrides into ${conf.sedFile}`);
  lines.push(`sed -i \"s|^#define ${conf.deviceKey} .*|#define ${conf.deviceKey} \\\"${name.replace(/\\/g, "\\\\").replace(/\"/g, '\\\"')}\\\"|\" ${conf.sedFile}`);

  if (variant === "cyd") {
    const touchToggle = disableTouch.checked ? "OFF" : "ON";
    const imageToggle = disableImageLoading.checked ? "OFF" : "ON";
    lines.push("");
    lines.push("# CYD feature toggles");
    lines.push(`# touch module: ${touchToggle}`);
    lines.push(`# image loading: ${imageToggle}`);
    lines.push(`idf.py -D CYD_ENABLE_TOUCH=${touchToggle} -D CYD_ENABLE_IMAGE_LOADING=${imageToggle} reconfigure`);
  }

  if (variant === "s3") {
    const ssid = shellEscapeSingle(wifiSsid.value.trim() || "MyWiFi");
    const pass = shellEscapeSingle(wifiPassword.value.trim() || "MyPassword");
    const host = shellEscapeSingle(slimHost.value.trim() || "192.168.1.100");
    const port = Number(slimPort.value) || 3483;

    lines.push(`sed -i \"s|^#define MA_WIFI_SSID .*|#define MA_WIFI_SSID \\\"${ssid.replace(/\\/g, "\\\\").replace(/\"/g, '\\\"')}\\\"|\" S3-BT/main/main.h`);
    lines.push(`sed -i \"s|^#define MA_WIFI_PASSWORD .*|#define MA_WIFI_PASSWORD \\\"${pass.replace(/\\/g, "\\\\").replace(/\"/g, '\\\"')}\\\"|\" S3-BT/main/main.h`);
    lines.push(`sed -i \"s|^#define MA_SLIMPROTO_HOST .*|#define MA_SLIMPROTO_HOST \\\"${host.replace(/\\/g, "\\\\").replace(/\"/g, '\\\"')}\\\"|\" S3-BT/main/main.h`);
    lines.push(`sed -i \"s|^#define MA_SLIMPROTO_PORT .*|#define MA_SLIMPROTO_PORT  ${port}|\" S3-BT/main/main.h`);
  }

  lines.push("");
  lines.push("# 2) Build and flash");
  for (const cmd of conf.buildCmd) {
    lines.push(cmd);
  }

  lines.push("");
  lines.push(`# Applied device name: '${escName}'`);

  output.value = lines.join("\n");
}

function downloadScript() {
  if (!output.value.trim()) {
    generateScript();
  }
  const variant = getVariant();
  const blob = new Blob([output.value], { type: "text/x-shellscript" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = variant === "s3" ? "build_flash_s3.sh" : "build_flash_cyd.sh";
  a.click();
  URL.revokeObjectURL(url);
}

document.querySelectorAll('input[name="variant"]').forEach((r) => {
  r.addEventListener("change", () => {
    deviceName.value = "";
    updateVariantUI();
    generateScript();
  });
});

document.querySelectorAll('input[name="cydPrebuilt"]').forEach((r) => {
  r.addEventListener("change", () => {
    if (getVariant() === "cyd") {
      const manifest = getPrebuiltManifestForVariant();
      installButton.setAttribute("manifest", manifest);
      checkFirmwareAvailability(manifest);
    }
  });
});

generateBtn.addEventListener("click", generateScript);
downloadBtn.addEventListener("click", downloadScript);
applyCustomScriptBtn.addEventListener("click", () => {
  applyCustomScript();
});
downloadManifestBtn.addEventListener("click", () => {
  downloadCustomManifest();
});
bootloaderFile.addEventListener("change", () => {
  applyCustomScript();
});
partitionFile.addEventListener("change", () => {
  applyCustomScript();
});
appFile.addEventListener("change", () => {
  applyCustomScript();
});
disableTouch.addEventListener("change", generateScript);
disableImageLoading.addEventListener("change", generateScript);

updateVariantUI();
generateScript();
