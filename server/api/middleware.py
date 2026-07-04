class SecurityHeadersMiddleware:
    """Adds a strict Content-Security-Policy on every response. The /api/stats dashboard
    uses only inline styles rendered by our own template, so 'unsafe-inline' is limited
    to styles; scripts are forbidden entirely."""

    def __init__(self, get_response):
        self.get_response = get_response

    def __call__(self, request):
        response = self.get_response(request)
        response.headers.setdefault(
            "Content-Security-Policy",
            "default-src 'none'; style-src 'unsafe-inline'; img-src 'self'; "
            "frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
        )
        response.headers.setdefault("X-Robots-Tag", "noindex")
        return response
