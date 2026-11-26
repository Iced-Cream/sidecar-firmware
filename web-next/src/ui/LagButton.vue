<!-- web-next/src/ui/LagButton.vue - "Lag now" (lib/lag.ts): one tap writes a marker line into the Sidecar's black box.
     In the header on every page (reachable while playing) and in Settings > About. -->
<script setup lang="ts">
import { computed, ref } from "vue";
import { useSidecar } from "../lib/useSidecar";
import { lagBlocked, sharedMarker } from "../lib/lag";
import { notify } from "../lib/notify";

defineProps<{ compact?: boolean }>();
const { cmd, paired, state } = useSidecar();
const marker = sharedMarker(cmd);
const blocked = computed(() => lagBlocked(state.value, paired.value));
const busy = ref(false);
async function press() {
  busy.value = true;
  try {
    if ((await marker.press()) === "sent") notify.ok("Lag marked", "The Sidecar wrote a marker into its black box.", "lag");
  } catch (e) {
    notify.error(e, "The lag marker was not taken");
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <button
    type="button"
    class="pill lag"
    :class="{ compact }"
    :disabled="busy || !!blocked"
    :title="paired ? 'Press when the Rytm feels slow: the Sidecar notes the moment in its black box' : 'Pair this browser first (Settings > Access)'"
    @click="press"
  >
    Lag now<span v-if="blocked" class="why"> ({{ blocked }})</span>
  </button>
</template>

<style scoped>
.lag { min-height: var(--target); }
.why { font-weight: 400; }
.lag.compact { padding: 0.375rem 0.875rem; font-size: 0.9375rem; background: color-mix(in srgb, var(--bg) 82%, transparent); }
</style>
