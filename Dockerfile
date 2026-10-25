# Reproducible CPU environment for fastzonal: builds the library with OpenMP, runs every test
# suite, and offers the benchmark runner as the entry point. CUDA is deliberately not included.
#
#   docker build --build-arg GIT_COMMIT=$(git rev-parse HEAD) -t fastzonal:bench .
#   docker run --rm -v "$PWD/benchmarks/fixtures:/work/benchmarks/fixtures" fastzonal:bench --quick
#
# The conda environment comes from conda-lock.yml (generated from environment.yml), so every
# Python and toolchain package is pinned; the .NET SDK comes from Microsoft's package feed.
# A tag rather than a digest: pin the digest with `docker pull` output before a release.
FROM condaforge/miniforge3:25.3.1-0

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl git wget \
    && wget -q https://packages.microsoft.com/config/ubuntu/22.04/packages-microsoft-prod.deb -O /tmp/ms.deb \
    && dpkg -i /tmp/ms.deb && rm /tmp/ms.deb \
    && apt-get update && apt-get install -y --no-install-recommends dotnet-sdk-10.0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
COPY environment.yml conda-lock.yml ./
RUN conda install -y -n base -c conda-forge conda-lock \
    && conda-lock install -n fastzonal conda-lock.yml \
    && conda clean -afy
ENV PATH=/opt/conda/envs/fastzonal/bin:$PATH \
    CC=/opt/conda/envs/fastzonal/bin/x86_64-conda-linux-gnu-gcc \
    CXX=/opt/conda/envs/fastzonal/bin/x86_64-conda-linux-gnu-g++ \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

ARG GIT_COMMIT=unknown
ENV FZ_GIT_COMMIT=$GIT_COMMIT
LABEL org.opencontainers.image.revision=$GIT_COMMIT
COPY . .
RUN cmake --preset release && cmake --build --preset release && ctest --preset release --output-on-failure \
    && cmake --build --preset release --target dotnet_native_copy \
    && dotnet test bindings/dotnet/FastZonal.slnx \
    && dotnet test benchmarks/csharp/FzBench.slnx \
    && python -m pytest benchmarks/tests -q

ENTRYPOINT ["bash", "benchmarks/run_all.sh"]
