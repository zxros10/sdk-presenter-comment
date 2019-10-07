#!/usr/bin/env python3
#   =======================================================================
#
# Copyright (C) 2018, Hisilicon Technologies Co., Ltd. All Rights Reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   1 Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#
#   2 Redistributions in binary form must reproduce the above copyright notice,
#     this list of conditions and the following disclaimer in the documentation
#     and/or other materials provided with the distribution.
#
#   3 Neither the names of the copyright holders nor the names of the
#   contributors may be used to endorse or promote products derived from this
#   software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#   =======================================================================
#

"""presenter server module"""

import os
import sys
import signal
import argparse
import logging

WEB_SERVER = None
APP_SERVER = None
RUN_SERVER = None
SERVER_TYPE = ""
#presenter server使用帮助信息
USAGE_INFO = "python3 prensenter_server.py [-h] --app \n\t\t\t\t{display}"

COMMON_MAP = {"web_server": "display.src.web",
              "app_server": "display.src.display_server"
            }

APP_CONF_MAP = {"display": COMMON_MAP}

#解析python3 prensenter_server.py [-h] --app \n\t\t\t\t{display} 的参数部分
def arg_parse():
    '''arg_parse'''
    global WEB_SERVER
    global APP_SERVER
    global SERVER_TYPE

    parser = argparse.ArgumentParser(usage=USAGE_INFO)
    parser.add_argument('--app', type=str, required=True,
                        choices=['display'],
                        help="Application type corresponding to Presenter Server.")
    args = parser.parse_args()
    SERVER_TYPE = args.app
    app_conf = APP_CONF_MAP.get(SERVER_TYPE, COMMON_MAP)
    #导入display/src/web.py
    WEB_SERVER = __import__(app_conf.get("web_server"), fromlist=True)
    #导入display/src/display_server.py
    APP_SERVER = __import__(app_conf.get("app_server"), fromlist=True)

#启动web
def start_app():
    global RUN_SERVER
    # start socket server for presenter agent communication
    RUN_SERVER = APP_SERVER.run()
    if RUN_SERVER is None:
        return False

    logging.info("presenter server starting, type: %s", SERVER_TYPE)
    # start web ui
    return WEB_SERVER.start_webapp()

#停止web和app
def stop_app():
    WEB_SERVER.stop_webapp()
    RUN_SERVER.stop_thread()

#杀死presenter server
def close_all_thread(signum, frame):
    '''close all thread of the process, and exit.'''
    logging.info("receive signal, signum:%s, frame:%s", signum, frame)
    stop_app()

    logging.info("presenter server exit by Ctrl + c")

    sys.exit()

#检查presenter server是否启动
def check_server_exist():
    pid = os.getpid()

    cmd = "ps -ef|grep -v {}|grep -w presenter_server|grep {}" \
            .format(pid, SERVER_TYPE)

    ret = os.system(cmd)

    return ret

def main_process():
    '''Main function entrance'''
    arg_parse()

    if check_server_exist() == 0:
        print("Presenter Server type \"%s\" already exist!" %(SERVER_TYPE))
        return True
    # process signal, when receive "Ctrl + c" signal,
    # stop all thead and exit the progress.
    signal.signal(signal.SIGINT, close_all_thread)
    signal.signal(signal.SIGTERM, close_all_thread)
    start_app()

    return True

#presenter server入口
if __name__ == "__main__":
    main_process()
