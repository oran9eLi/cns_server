import React from "react";
import ReactDOM from "react-dom/client";
import { ConfigProvider } from "antd";
import type { Locale } from "antd/es/locale/index.js";
import zhCNRaw from "antd/locale/zh_CN.js";

import { App } from "./App.js";
import "./styles.css";

const zhCN = ((zhCNRaw as unknown as { default?: Locale }).default ?? zhCNRaw) as Locale;

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <ConfigProvider
      locale={zhCN}
      theme={{
        token: {
          colorPrimary: "#132B88",
          borderRadius: 6,
          fontFamily:
            '-apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft YaHei", sans-serif'
        },
        components: {
          Card: {
            borderRadiusLG: 6
          },
          Button: {
            borderRadius: 6
          }
        }
      }}
    >
      <App />
    </ConfigProvider>
  </React.StrictMode>
);
