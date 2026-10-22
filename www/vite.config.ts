import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  build: { target: 'es2022', sourcemap: false },
  server: {
    // Use the same Gateway API during development; production assets use their own origin.
    proxy: {
      '/api': {
        target: process.env.UAV_DEV_GATEWAY || 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
      '/healthz': {
        target: process.env.UAV_DEV_GATEWAY || 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
})
