# Plain-HTTP view of the controller UI, for browsers that will not accept
# its self-signed certificate. See controller_proxy.py for implementation
# and environment details.
FROM python:3.12-alpine

COPY controller_proxy.py /app/controller_proxy.py

ENV CONTROLLER_HOST=controller \
    CONTROLLER_PORT=8043 \
    PROXY_BIND=0.0.0.0 \
    PROXY_PORT=8090

EXPOSE 8090

CMD ["python3", "-u", "/app/controller_proxy.py"]