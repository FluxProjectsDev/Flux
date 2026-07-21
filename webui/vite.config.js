import { fileURLToPath, URL } from 'node:url'

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
//import vueDevTools from 'vite-plugin-vue-devtools'
import tailwindcss from '@tailwindcss/vite'
import mkcert from 'vite-plugin-mkcert'
import { ViteMinifyPlugin } from 'vite-plugin-minify'

// https://vite.dev/config/
//
// A function config so the dev-only plugins are excluded from a production build rather than
// merely inert in it. `command` is 'serve' for `vite` (dev) and 'build' for `vite build`.
export default defineConfig(({ command }) => {
  const isDev = command === 'serve'

  return {
    plugins: [
      vue(),
      //vueDevTools(),
      tailwindcss(),
      ViteMinifyPlugin({}),
      // mkcert provisions a local TLS certificate for the HTTPS dev server. It has no place in a
      // production build: the WebUI ships as root's local page inside the module and is never
      // served over the network, so a build-time cert step would touch the filesystem (and, on a
      // cold cache, the network) to produce something nothing uses. Dev only.
      ...(isDev ? [mkcert()] : []),
    ],
    server: {
      https: true,
    },
    build: {
      // No source maps in production. A source map republishes the original, unminified source and
      // the file layout beside the shipped bundle — shipping it defeats the point of shipping
      // minified code. This is Vite's default; it is stated explicitly so a future edit cannot
      // turn it on without deleting the comment that says why not, and verify-webui-inventory.sh
      // fails the build if a .map ever reaches dist/.
      sourcemap: false,
      // Minify JavaScript and CSS. esbuild is Vite's default minifier; named here so the choice
      // is audited rather than implicit.
      minify: 'esbuild',
      cssMinify: true,
    },
    resolve: {
      alias: {
        '@': fileURLToPath(new URL('./src', import.meta.url)),
      },
    },
  }
})
