// Canonical-host shim: 301 www -> apex, everything else falls through to static assets.
export default {
  fetch(request, env) {
    const url = new URL(request.url);
    if (url.hostname === "www.alloylogger.com") {
      url.hostname = "alloylogger.com";
      return Response.redirect(url.toString(), 301);
    }
    return env.ASSETS.fetch(request);
  },
};
