# SSAS - Simple Smart Automotive Software
# Copyright (C) 2021 Parai Wang <parai@foxmail.com>

import os
import json
from .helper import *
from .SomeIp import Gen_SomeIp
from .SoAd import Gen_SoAd

__all__ = ['Gen']

__GENNET__ = {
    'SomeIp': Gen_SomeIp,
    'SoAd': Gen_SoAd,
}


def Gen_Net(cfg, dir):
    for mod in cfg['Modules']:
        if mod['class'] in __GENNET__:
            __GENNET__[mod['class']](mod, dir)
        else:
            raise Exception('unknown class %s' % (mod['class']))


def ProcSomeIp(cfg, mod):
    sock = {'name': 'SD_MULTICAST',
            'server': mod['SD']['multicast'] + ':30490', 'protocol': 'UDP', 'up': 'SD', 'RxPduId': 'SD_RX_PID_MULTICAST'}
    cfg['sockets'].append(sock)
    sock = {'name': 'SD_UNICAST', 'server': 'NULL:30490',
            'protocol': 'UDP', 'up': 'SD', 'RxPduId': 'SD_RX_PID_UNICAST'}
    cfg['sockets'].append(sock)
    for service in mod.get('servers', []):
        name = 'SOMEIP_%s' % (service['name'].upper())
        if 'reliable' in service:
            protocol = 'TCP'
            port = service['reliable']
        else:
            protocol = 'UDP'
            port = service['unreliable']
        sock = {'name': name, 'server': 'NULL:%s' % (port),
                'protocol': protocol, 'up': 'SOMEIP', 'RxPduId': 'SOMEIP_RX_PID_%s' % (name)}
        if 'reliable' in service:
            sock['listen'] = service['listen'] if 'listen' in service else 3
        cfg['sockets'].append(sock)
    for service in mod.get('clients',[]):
        name = 'SOMEIP_%s' % (service['name'].upper())
        if 'reliable' in service:
            protocol = 'TCP'
            port = service['reliable']
        else:
            protocol = 'UDP'
            port = service['unreliable']
        sock = {'name': name, 'client': 'NULL:%s' % (port),
                'protocol': protocol, 'up': 'SOMEIP', 'RxPduId': 'SOMEIP_RX_PID_%s' % (name)}
        cfg['sockets'].append(sock)


def ProcSoAd(netCfg, dir):
    cfg = None
    for mod in netCfg['Modules']:
        if mod['class'] == 'SoAd':
            cfg = mod['class']
    if cfg == None:
        cfg = {'class': 'SoAd', 'sockets': []}
        netCfg['Modules'].append(cfg)
    for mod in netCfg['Modules']:
        if mod['class'] == 'SomeIp':
            ProcSomeIp(cfg, mod)
    with open('%s/Network.json' % (dir), 'w') as f:
        json.dump(netCfg, f, indent=2)

RandomPort = 60000
def GenAnitTestForSomeIp(cfgj):
    global RandomPort
    dir = os.path.join(os.path.dirname(cfgj), 'GENT')
    os.makedirs(dir, exist_ok=True)
    with open(cfgj) as f:
        cfg = json.load(f)
    someIpCfg = None
    for mod in cfg['Modules']:
        if mod['class'] == 'SomeIp':
            someIpCfg = mod
            servers = []
            clients = []
            if 'servers' in mod:
                servers = mod['servers']
                del mod['servers']
            if 'clients' in mod:
                clients = mod['clients']
                del mod['clients']
            for ser in servers:
                if 'unreliable' in ser:
                    ser['unreliable'] = RandomPort
                    RandomPort += 1
            mod['clients'] = servers
            mod['servers'] = clients
    if someIpCfg:
        cfg['Modules'] = [someIpCfg]
    ProcSoAd(cfg, dir)
    Gen_Net(cfg, dir)

def Gen(cfgj):
    dir = os.path.join(os.path.dirname(cfgj), 'GEN')
    os.makedirs(dir, exist_ok=True)
    with open(cfgj) as f:
        cfg = json.load(f)
    ProcSoAd(cfg, dir)
    Gen_Net(cfg, dir)
    GenAnitTestForSomeIp(cfgj)
