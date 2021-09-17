FROM postgres:13
RUN \
apt-get update && apt-get -y upgrade && \
apt-get -y install git postgresql-server-dev-13 make gcc && \
git clone https://github.com/pgaudit/set_user /opt/set_user && \
make -C /opt/set_user USE_PGXS=1 && make -C /opt/set_user USE_PGXS=1 install
