import react from "@vitejs/plugin-react";
import { defineConfig, loadEnv } from "vite";

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "");
  const apiTarget = env.CNS_DEV_PROXY_TARGET || "http://127.0.0.1:3100";

  return {
    plugins: [react()],
    server: {
      host: "127.0.0.1",
      port: 5173,
      proxy: {
        "/api": {
          target: apiTarget,
          changeOrigin: true
        },
        "/ws": {
          target: apiTarget,
          changeOrigin: true,
          ws: true
        },
        "/__dev": {
          target: apiTarget,
          changeOrigin: true
        }
      }
    }
  };
});
