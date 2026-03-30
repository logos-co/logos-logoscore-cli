# Stage 1: Build
FROM nixos/nix:2.34.1 AS builder
RUN echo "experimental-features = nix-command flakes" >> /etc/nix/nix.conf
WORKDIR /app

COPY . /src
RUN nix build '/src#portable' --out-link ./logoscore --no-write-lock-file
RUN nix build 'github:logos-co/logos-liblogos' --out-link ./logos --no-write-lock-file
RUN nix build 'github:logos-co/logos-package-manager-module/4c49df4c42bfb5bc4a6a27e526ab9755baa064a9#cli' --out-link ./package-manager --refresh --no-write-lock-file

RUN mkdir modules \
    && ./package-manager/bin/lgpm --modules-dir ./modules/ install logos-waku-module \
    && ./package-manager/bin/lgpm --modules-dir ./modules/ install logos-storage-module \
    && ./package-manager/bin/lgpm --modules-dir ./modules/ install logos-blockchain-module \
    && ln -sf /app/modules/libpq.so /app/modules/libpq.so.5

RUN if [ -d ./modules/liblogos_blockchain_module/circuits ]; then \
      find ./modules/liblogos_blockchain_module/circuits -name "witness_generator" -exec chmod u+x {} \; ; \
      chmod u+x ./modules/liblogos_blockchain_module/circuits/prover 2>/dev/null || true ; \
      chmod u+x ./modules/liblogos_blockchain_module/circuits/verifier 2>/dev/null || true ; \
    fi

RUN mkdir /runtime-store \
    && (nix-store -qR ./logos; nix-store -qR ./logoscore; nix-store -qR ./package-manager) \
       | sort -u | while read path; do \
         cp -a "$path" /runtime-store/; \
       done

RUN mkdir -p /app-final/logos/bin /app-final/logos/lib /app-final/logos/modules \
    && cp -rL ./logos/lib/* /app-final/logos/lib/ \
    && cp -rL ./logos/modules/* /app-final/logos/modules/ 2>/dev/null || true \
    && cp -rL ./logoscore/bin/logoscore /app-final/logos/bin/logoscore \
    && cp -rL ./logos/bin/logos_host /app-final/logos/bin/logos_host \
    && cp -r ./modules /app-final/modules

# Stage 2: Runtime
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends curl && rm -rf /var/lib/apt/lists/*
COPY --from=builder /runtime-store /nix/store
COPY --from=builder /app-final/logos /app/logos
COPY --from=builder /app-final/modules /app/modules
RUN mkdir -p /etc/logos/blockchain
WORKDIR /app

CMD ["./logos/bin/logoscore", "-m", "./modules", "-l", "waku_module,storage_module", \
     "-c", "waku_module.initWaku(@waku_config.json)", \
     "-c", "waku_module.startWaku()", \
     "-c", "storage_module.init(@storage_config.json)", \
     "-c", "storage_module.start()", \
     "-c", "storage_module.importFiles('/tmp/storage_files')"]