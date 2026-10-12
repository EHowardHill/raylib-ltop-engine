import react from '@vitejs/plugin-react';

export default {
  plugins: [react()],
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
    watch: { ignored: ['**/src-tauri/**'] }
  },
  build: { outDir: 'dist', target: 'safari15' }
};