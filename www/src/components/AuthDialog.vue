<script setup lang="ts">
import { nextTick, ref, watch } from 'vue'
import { ArrowRight, KeyRound, LoaderCircle, ShieldCheck, X } from '@lucide/vue'
const props = defineProps<{ open: boolean; busy: boolean; error: string }>()
const emit = defineEmits<{ close: []; login: [token: string] }>()
const dialog = ref<HTMLDialogElement>(),
  input = ref<HTMLInputElement>(),
  token = ref('')
watch(
  () => props.open,
  async (open) => {
    await nextTick()
    if (open) {
      if (!dialog.value?.open) dialog.value?.showModal()
      input.value?.focus()
    } else {
      dialog.value?.close()
      token.value = ''
    }
  },
)
function submit() {
  if (!token.value.trim() || props.busy) return
  emit('login', token.value.trim())
  token.value = ''
}
</script>

<template>
  <dialog
    ref="dialog"
    class="auth-dialog"
    aria-labelledby="auth-title"
    @cancel.prevent="!busy && emit('close')"
  >
    <button
      class="icon-button dialog-close"
      :disabled="busy"
      aria-label="关闭登录窗口"
      @click="emit('close')"
    >
      <X :size="19" />
    </button>
    <div class="dialog-symbol"><KeyRound :size="25" /></div>
    <span class="eyebrow">DEVICE ACCESS</span>
    <h2 id="auth-title">连接你的设备</h2>
    <p class="dialog-description">使用管理员或观看凭据登录，接入设备的实时画面与运行状态。</p>
    <form @submit.prevent="submit">
      <label for="credential">设备接入凭据</label>
      <input
        id="credential"
        ref="input"
        v-model="token"
        type="password"
        autocomplete="off"
        placeholder="请输入接入凭据"
        :disabled="busy"
        required
        :aria-invalid="Boolean(error)"
        aria-describedby="auth-error"
      />
      <p v-if="error" id="auth-error" class="form-error" role="alert">{{ error }}</p>
      <button
        class="button button-primary login-submit"
        type="submit"
        :disabled="busy || !token.trim()"
      >
        <LoaderCircle v-if="busy" class="spin" :size="17" /><span>{{
          busy ? '正在验证' : '连接设备'
        }}</span
        ><ArrowRight v-if="!busy" :size="17" />
      </button>
    </form>
    <p class="auth-footnote"><ShieldCheck :size="14" />登录信息仅保留在当前页面会话中</p>
  </dialog>
</template>
