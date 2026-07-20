export function getConfigPathFromEnv(): string | undefined {
  return process.env.CNS_BACKEND_CONFIG;
}
