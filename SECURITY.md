# Security Policy

This is an independent downstream mod maintained by `ismoss`. It is not affiliated with MindReset or the upstream `wegooo-cell/read-pico-reader` project. Report problems in this mod here, not through MindReset's or the upstream project's private channels.

Use this repository's [private vulnerability report](https://github.com/ismoss/mossread/security/advisories/new) when available. If private reporting is disabled, open an issue asking for a private contact channel without posting exploit details or private data.

Include the affected commit or release, component, hardware revision if known, impact, and safe reproduction details. Remove credentials, WiFi network names if sensitive, LAN addresses, full device identifiers, book content, backups, and personal data. Do not publish vulnerability details in issues or pull requests before a fix is available.

Remote monitor is a local-network service that is off by default and intended for brief use on a trusted network. Do not expose it to the public Internet. Anyone who can reach an enabled instance and obtain the current session token may view the screen or inject touch input; treat the tokenized URL as a password.

Ordinary bugs, documentation corrections, and non-sensitive hardware questions belong in this repository's issue tracker. See [Support](SUPPORT.md) and [Privacy](PRIVACY.md) for other help.

## Remote monitor

Remote monitor is off at every boot. Enabling it creates a random session-only access token shown with the service URL. The token stays in the URL fragment (which browsers do not send to the server) and is passed to the frame and touch endpoints in a request header; those endpoints reject requests without it. The token is not stored in NVS or written by the application to logs. The ten-minute idle timeout disables the service.

The transport is plain HTTP on the local network, not TLS. A passive network observer can capture the token and session traffic, so enable it only on a trusted LAN, never expose port 8123 through router forwarding or a public tunnel, and disable it when finished.
