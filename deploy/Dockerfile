# ERServer — Windows Server Core container (§20 of the masterplan).
#
# Build context: repo root
# Usage:
#   docker build -f deploy/Dockerfile -t earthrise-server .
#   docker run --rm -p 7777:7777/udp \
#       -e ER_DB_CONNSTR="Server=host.docker.internal,1433;Database=EarthRise;User Id=...;Password=...;Encrypt=yes" \
#       -e ER_SERVER_PEPPER="<base64-pepper>" \
#       earthrise-server
#
# Notes:
#   - SQL Server is an EXTERNAL network service — never containerised (§20).
#   - The ODBC Driver 18 is installed below from the official Microsoft package.
#   - Production: one pod per shard, UDP LoadBalancer, Windows node pool (§20).

FROM mcr.microsoft.com/windows/servercore:ltsc2022 AS build
SHELL ["cmd", "/S", "/C"]

# Install Visual Studio Build Tools (C++ workload) — required to compile the solution.
# In a real CI pipeline this layer is pre-baked into a custom build image.
ADD https://aka.ms/vs/17/release/vs_buildtools.exe C:\vs_buildtools.exe
RUN C:\vs_buildtools.exe --quiet --wait --norestart --nocache \
    --add Microsoft.VisualStudio.Workload.VCTools \
    --add Microsoft.VisualStudio.Component.Windows11SDK.26100 \
    --includeRecommended \
    && del C:\vs_buildtools.exe

WORKDIR C:\src
COPY . .

# Build ERServer in Release|x64.
RUN "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" \
    EarthRise.slnx \
    /p:Configuration=Release \
    /p:Platform=x64 \
    /t:ERServer \
    /m

# -----------------------------------------------------------------------
# Runtime image — lean Server Core
# -----------------------------------------------------------------------
FROM mcr.microsoft.com/windows/servercore:ltsc2022 AS runtime
SHELL ["cmd", "/S", "/C"]

# Install ODBC Driver 18 for SQL Server.
ADD https://go.microsoft.com/fwlink/?linkid=2249006 C:\msodbcsql18.msi
RUN msiexec /i C:\msodbcsql18.msi /quiet /norestart IACCEPTMSODBCSQLLICENSETERMS=YES \
    && del C:\msodbcsql18.msi

WORKDIR C:\app
COPY --from=build C:\src\x64\Release\ERServer.exe .

# UDP port for client connections.
EXPOSE 7777/udp

# Connection string and server pepper injected at runtime via env / k8s Secret.
ENV ER_DB_CONNSTR=""
ENV ER_SERVER_PEPPER=""
ENV ER_LISTEN_PORT=7777

ENTRYPOINT ["C:\\app\\ERServer.exe"]
