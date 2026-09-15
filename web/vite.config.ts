import { defineConfig } from 'vite';

export default defineConfig({
  server: {
    strictPort: true,
    port: 5173,
    proxy: { '/lab': { target: 'http://127.0.0.1:8766', rewrite: path => path.slice(4) } },
  },
});
