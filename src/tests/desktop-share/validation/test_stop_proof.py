"""Adversarial stop proof checks using self-contained synthetic captures.

Outputs are opt-in and must be a new path, so reruns cannot rewrite evidence.

Two sets are kept separate on purpose:
  * the original 26 rejected mutations plus the valid proof (27 per capture);
  * the post-completion set that failed the earlier analyser: a missing post-stop
    publisher role, a missing/zero local UDP port, and explicit same-stream
    samples 1000 ms after stop completion that still publish, carry outbound
    bytes or queue media;
  * the round-6 set: samples whose stream identity is missing, malformed or
    contradictions between the top level and the pipeline snapshot, samples
    with no numeric timestamp, samples whose array position disagrees with the
    largest timestamp, and the stop+0ms completion boundary.
"""
import argparse
import copy
import json
import unittest
from analyze_stop import evaluate


def fixture():
    cap = dict(startedAtEpochMs=1000, endedAtEpochMs=11000,
        lastUplinkEpochMs=3000, lastDownlinkEpochMs=3100, ztPeer='10.10.10.3',port=18500,
        timestampBasis='SO_TIMESTAMPNS kernel packet timestamps; read epochs separate',
        buckets=[], packetEvents=[],kernelPacketStatistics={'media':dict(kernelPackets=2,kernelDrops=0)},
        uplink=dict(registerDatagrams=2,endpoints={'10.10.10.3:50001':dict(datagrams=2)}),
        downlink=dict(registerAckDatagrams=2,endpoints={'10.10.10.3:50002':dict(datagrams=2)}),
        independentMediaFlows={
            'up:10.10.10.3:50001:epoch=1':dict(lastKernelEpochMs=3000,lastReadEpochMs=3001,videoDatagrams=1),
            'down:10.10.10.3:50002:epoch=1':dict(lastKernelEpochMs=3100,lastReadEpochMs=3101,videoDatagrams=1)})
    pub = dict(stopRequestedEpochMs=3000,stopCompletedEpochMs=4000,
        publisherSamples=[dict(epochMs=2900,pipeline=dict(publisher=True,publishEpoch='1',localUdpPort=50001))],
        stats=dict(localAddress='10.10.10.3',serverUdpPort=18500,publishEpoch='1',localUdpPort=50001),
        statsAfterStop=dict(outboundBytes=0,outboundQueued=0,publisher=False))
    cap['packetEvents']=[dict(epochMs=3000,readEpochMs=3001,direction='up',endpoint='10.10.10.3:50001'),
                         dict(epochMs=3100,readEpochMs=3101,direction='down',endpoint='10.10.10.3:50002')]
    return cap,pub


def variants(cap,pub,offset=0):
    def variant(name, modify):
        c,p=copy.deepcopy(cap),copy.deepcopy(pub);modify(c,p);return name,c,p
    flow=next(iter(cap['independentMediaFlows']))
    return [
        variant('wrong-peer',lambda c,p:c.update(ztPeer='10.10.10.99')),
        variant('wrong-port',lambda c,p:c.update(port=18100)),
        variant('wrong-endpoint-address',lambda c,p:c['independentMediaFlows'].update(
            {flow.replace('10.10.10.3','10.10.10.99'):c['independentMediaFlows'].pop(flow)})),
        variant('wrong-endpoint-port',lambda c,p:c['independentMediaFlows'].update(
            {flow.rsplit(':epoch=',1)[0].rsplit(':',1)[0]+':1:epoch=1':c['independentMediaFlows'].pop(flow)})),
        variant('wrong-flow-epoch',lambda c,p:c['independentMediaFlows'].update(
            {flow.rsplit('=',1)[0]+'=99':c['independentMediaFlows'].pop(flow)})),
        variant('all-flows-wrong-epoch',lambda c,p:c.update(independentMediaFlows={
            n.rsplit('=',1)[0]+'=99':v for n,v in c['independentMediaFlows'].items()})),
        variant('missing-register',lambda c,p:c['uplink'].pop('registerDatagrams')),
        variant('zero-register',lambda c,p:c['uplink'].update(registerDatagrams=0)),
        variant('missing-register-ack',lambda c,p:c['downlink'].pop('registerAckDatagrams')),
        variant('zero-register-ack',lambda c,p:c['downlink'].update(registerAckDatagrams=0)),
        variant('read-time-only',lambda c,p:c.update(timestampBasis='read time only')),
        variant('missing-read-time',lambda c,p:c['independentMediaFlows'][flow].pop('lastReadEpochMs')),
        variant('read-after-stop',lambda c,p:c['independentMediaFlows'][flow].update(lastReadEpochMs=p['stopCompletedEpochMs']+2000)),
        variant('packet-up-after-stop',lambda c,p:c.update(packetEvents=[dict(epochMs=p['stopCompletedEpochMs']+2000,
            readEpochMs=p['stopCompletedEpochMs']+2001,direction='up',endpoint='10.10.10.3:50001')])),
        variant('packet-down-after-stop',lambda c,p:c.update(packetEvents=[dict(epochMs=p['stopCompletedEpochMs']+2000,
            readEpochMs=p['stopCompletedEpochMs']+2001,direction='down',endpoint='10.10.10.3:50002')])),
        variant('missing-events',lambda c,p:c.pop('packetEvents')),
        variant('empty-events',lambda c,p:c.update(packetEvents=[])),
        variant('post-stop-bytes',lambda c,p:p['statsAfterStop'].update(outboundBytes=1)),
        variant('post-stop-queued',lambda c,p:p['statsAfterStop'].update(outboundQueued=1)),
        variant('missing-post-stop',lambda c,p:p.pop('statsAfterStop')),
        variant('missing-post-stop-bytes',lambda c,p:p['statsAfterStop'].pop('outboundBytes')),
        variant('missing-post-stop-queued',lambda c,p:p['statsAfterStop'].pop('outboundQueued')),
        variant('missing-publisher-stats',lambda c,p:p.pop('stats')),
        variant('inactive-before-stop',lambda c,p:p['publisherSamples'][-1]['pipeline'].update(publisher=False)),
        variant('capture-less-than-five-seconds',lambda c,p:c.update(endedAtEpochMs=p['stopCompletedEpochMs']+offset+4900)),
        variant('kernel-loss',lambda c,p:c['kernelPacketStatistics']['media'].update(kernelDrops=1)),
    ]


def post_completion_variants(cap,pub):
    """The six conditions that silently passed the earlier analyser, plus the
    round-6 identity, timestamp, order and completion-boundary counterexamples."""
    def variant(name, modify):
        # These mutations only touch the publisher side; the capture is shared
        # read-only so a 30-minute capture is not copied for every case.
        p=copy.deepcopy(pub);modify(cap,p);return name,cap,p
    def post(field,value,**options):
        def change(c,p):
            pipeline=copy.deepcopy(p['statsAfterStop'])
            epoch=options.get('epoch','same');port=options.get('port','same')
            if options.get('drop_epoch'):pipeline.pop('publishEpoch',None)
            else:pipeline['publishEpoch']=p['stats']['publishEpoch'] if epoch=='same' else epoch
            if options.get('drop_port'):pipeline.pop('localUdpPort',None)
            else:pipeline['localUdpPort']=p['stats']['localUdpPort'] if port=='same' else port
            pipeline[field]=value
            sample={'pipeline':pipeline}
            if not options.get('drop_timestamp'):
                sample['epochMs']=p['stopCompletedEpochMs']+options.get('delta',1000)
            sample[field]=value
            if options.get('sample_epoch') is not None:
                sample['publishEpoch']=(p['stats']['publishEpoch']
                    if options['sample_epoch']=='current' else options['sample_epoch'])
            if options.get('sample_port') is not None:
                sample['localUdpPort']=(p['stats']['localUdpPort']
                    if options['sample_port']=='current' else options['sample_port'])
            p['publisherSamples'].append(sample)
        return change
    def order(c,p):
        # The newest pre-stop sample says "stopped"; an older active sample is
        # appended at the end of the array and must not revive activity.
        p['publisherSamples'][-1]['pipeline']['publisher']=False
        p['publisherSamples'].append(dict(
            epochMs=p['stopRequestedEpochMs']-1900,
            pipeline=dict(publisher=True,publishEpoch=p['stats']['publishEpoch'],
                          localUdpPort=p['stats']['localUdpPort'])))
    def ambiguous(c,p):
        latest=max(s['epochMs'] for s in p['publisherSamples']
                   if isinstance(s.get('epochMs'),(int,float))
                   and p['stopRequestedEpochMs'] is not None
                   and s['epochMs']<=p['stopRequestedEpochMs'])
        sample=copy.deepcopy([s for s in p['publisherSamples'] if s['epochMs']==latest][-1])
        sample['pipeline']['publisher']=False
        p['publisherSamples'].append(sample)
    return [
        variant('missing-post-completion-role',lambda c,p:p['statsAfterStop'].pop('publisher')),
        variant('missing-local-udp-port',lambda c,p:p['stats'].pop('localUdpPort')),
        variant('zero-local-udp-port',lambda c,p:p['stats'].update(localUdpPort=0)),
        variant('post-completion-still-publishing',post('publisher',True)),
        variant('post-completion-new-outbound-bytes',post('outboundBytes',1000)),
        variant('post-completion-new-queued-media',post('outboundQueued',1)),
        # A post-stop sample that belongs to a newer generation is not the
        # stopped flow and must not be misreported as its activity.
        variant('post-completion-other-generation-is-not-current',lambda c,p:p['publisherSamples'].append(dict(
            epochMs=p['stopCompletedEpochMs']+1000,
            pipeline=dict(publisher=True,publishEpoch='99',localUdpPort=50999)))),
        # Round 6: identity must be strictly valid to speak for the stream.
        # Missing, malformed or conflicting identity can never be reclassified
        # as "another generation" and silently ignored.
        variant('post-completion-zero-port-still-publishing',post('publisher',True,port=0)),
        variant('post-completion-zero-port-new-outbound-bytes',post('outboundBytes',1000,port=0)),
        variant('post-completion-invalid-port-still-publishing',post('publisher',True,port=70000)),
        variant('post-completion-missing-port-still-publishing',post('publisher',True,drop_port=True)),
        variant('post-completion-zero-epoch-still-publishing',post('publisher',True,epoch=0)),
        variant('post-completion-missing-epoch-still-publishing',post('publisher',True,drop_epoch=True)),
        variant('post-completion-conflicting-identity-still-publishing',
                post('publisher',True,epoch='99',sample_epoch='current',sample_port='current')),
        # Round 6: no numeric timestamp means the sample cannot be placed on
        # either side of stop completion; it must not be skipped.
        variant('post-completion-missing-timestamp-still-publishing',post('publisher',True,drop_timestamp=True)),
        variant('post-completion-missing-timestamp-new-outbound-bytes',post('outboundBytes',1000,drop_timestamp=True)),
        # Round 6: array position is not chronology.
        variant('pre-stop-latest-inactive-with-older-active-appended',order),
        variant('pre-stop-ambiguous-latest-state',ambiguous),
        # Round 6: stop+0ms is the completion boundary; +1ms is after it.
        variant('post-completion-activity-at-exact-stop',post('publisher',True,delta=0)),
        variant('post-completion-activity-1ms-after-stop',post('publisher',True,delta=1)),
    ]


class StopProofTests(unittest.TestCase):
    results=[]
    def exercise(self,label,c,p,offset=0,error=150,peer=None,port=None):
        control=evaluate(c,p,offset,error,expected_peer=peer,expected_port=port)
        self.results.append(dict(name=label+'/valid',expected=True,result=control))
        self.assertTrue(control['ok'],control)
        for name,mutated,publisher in variants(c,p,offset):
            with self.subTest(label=label,variant=name):
                result=evaluate(mutated,publisher,offset,error,expected_peer=peer,expected_port=port)
                self.results.append(dict(name=label+'/'+name,expected=False,result=result))
                self.assertFalse(result['ok'],result)
        # The other-generation sample is allowed only because it is excluded by
        # identity; the proof itself must still be confirmed.
        other=[x for x in post_completion_variants(c,p) if x[0]=='post-completion-other-generation-is-not-current'][0]
        result=evaluate(other[1],other[2],offset,error,expected_peer=peer,expected_port=port)
        self.results.append(dict(name=label+'/'+other[0],expected=True,result=result))
        self.assertTrue(result['ok'],result)
        for name,mutated,publisher in post_completion_variants(c,p):
            if name=='post-completion-other-generation-is-not-current':
                continue
            with self.subTest(label=label,variant=name):
                result=evaluate(mutated,publisher,offset,error,expected_peer=peer,expected_port=port)
                self.results.append(dict(name=label+'/'+name,expected=False,result=result))
                self.assertFalse(result['ok'],result)
                self.assertTrue(result['missingEvidence'] or result['rejectedEvidence'],
                                'a failed proof must say why: %r' % result)
    def test_synthetic(self): self.exercise('synthetic',*fixture())
    def _post_sample(self,pub,delta=1000,pipeline=None,sample=None):
        pipe=dict(publisher=True,publishEpoch=pub['stats']['publishEpoch'],
                  localUdpPort=pub['stats']['localUdpPort'])
        pipe.update(pipeline or {})
        pipe={k:v for k,v in pipe.items() if v is not None}
        entry={'epochMs':pub['stopCompletedEpochMs']+delta,'pipeline':pipe}
        entry.update(sample or {})
        return entry
    def test_stop_completion_boundary_semantics(self):
        cap,pub=fixture()
        # stop+0ms: the stop and the send share one millisecond, so activity is
        # unconfirmed (never rejected, never confirmed); same stream identity.
        for delta,status,reason,evidence in (
                (0,'unconfirmed','publisherSamples.activity-at-stop-completion@4000','missingEvidence'),
                (1,'rejected','post-stop-activity@4001','rejectedEvidence')):
            p=copy.deepcopy(pub)
            p['publisherSamples'].append(self._post_sample(p,delta))
            r=evaluate(cap,p,0,150)
            self.assertFalse(r['ok'],r)
            self.assertEqual(r['status'],status,r)
            self.assertIn(reason,r[evidence],r)
            self.assertEqual(r['postStopSamples'][-1]['boundary'],delta==0,r)
            self.assertEqual(r['postStopSamples'][-1]['identity'],'same',r)
        # An inactive boundary sample is neutral: the clean proof still passes.
        p=copy.deepcopy(pub)
        sample=self._post_sample(p,0,pipeline=dict(publisher=False))
        p['publisherSamples'].append(sample)
        r=evaluate(cap,p,0,150)
        self.assertTrue(r['ok'],r)
        self.assertFalse(r['postStopActivityObserved'],r)
    def test_invalid_sample_identity_is_unconfirmed_not_another_generation(self):
        cap,pub=fixture()
        cases={
            'port-zero':self._post_sample(pub,pipeline=dict(localUdpPort=0)),
            'port-out-of-range':self._post_sample(pub,pipeline=dict(localUdpPort=70000)),
            'port-missing':self._post_sample(pub,pipeline=dict(localUdpPort=None)),
            'epoch-zero':self._post_sample(pub,pipeline=dict(publishEpoch='0')),
            'epoch-missing':self._post_sample(pub,pipeline=dict(publishEpoch=None)),
            'conflict':self._post_sample(
                pub,pipeline=dict(publishEpoch='99'),
                sample=dict(publishEpoch=pub['stats']['publishEpoch'],
                            localUdpPort=pub['stats']['localUdpPort'])),
        }
        for name,sample in cases.items():
            p=copy.deepcopy(pub);p['publisherSamples'].append(sample)
            r=evaluate(cap,p,0,150)
            self.assertFalse(r['ok'],(name,r))
            self.assertEqual(r['status'],'unconfirmed',(name,r))
            self.assertEqual(r['postStopSamples'][-1]['identity'],'unconfirmed',(name,r))
            self.assertNotIn('post-stop-activity@4500',r['rejectedEvidence'],(name,r))
            # The activity is observed but cannot be attributed; it is not
            # silently dropped (the proof still fails as unconfirmed).
            self.assertTrue(r['postStopActivityObserved'],(name,r))
    def test_activity_without_timestamp_is_unconfirmed(self):
        cap,pub=fixture()
        p=copy.deepcopy(pub)
        sample=self._post_sample(p);sample.pop('epochMs')
        p['publisherSamples'].append(sample)
        r=evaluate(cap,p,0,150)
        self.assertFalse(r['ok'],r)
        self.assertEqual(r['status'],'unconfirmed',r)
        self.assertIn('publisherSamples.timestamp-missing',r['missingEvidence'],r)
        self.assertIsNone(r['postStopSamples'][-1]['epochMs'],r)
        self.assertTrue(r['postStopSamples'][-1]['active'],r)
    def test_pre_stop_activity_uses_latest_timestamp_not_array_order(self):
        cap,pub=fixture()
        pipe=lambda active:dict(publisher=active,publishEpoch='1',localUdpPort=50001)
        p=copy.deepcopy(pub)
        p['publisherSamples']=[dict(epochMs=2900,pipeline=pipe(True)),
                               dict(epochMs=2500,pipeline=pipe(True)),
                               dict(epochMs=2800,pipeline=pipe(False))]
        r=evaluate(cap,p,0,150)
        self.assertTrue(r['ok'],r)
        self.assertEqual(r['publisherActiveSampleEpochMs'],2900,r)
        # The newest sample says stopped; an older active sample appended at the
        # end of the array must not revive "was publishing before the stop".
        p['publisherSamples'][0]['pipeline']['publisher']=False
        p['publisherSamples'].append(dict(epochMs=1000,pipeline=pipe(True)))
        r=evaluate(cap,p,0,150)
        self.assertFalse(r['ok'],r)
        self.assertIn('publisher-not-active-before-stop',r['rejectedEvidence'],r)
    def test_contradictory_latest_pre_stop_state_is_unconfirmed(self):
        cap,pub=fixture()
        pipe=lambda active:dict(publisher=active,publishEpoch='1',localUdpPort=50001)
        p=copy.deepcopy(pub)
        p['publisherSamples']=[dict(epochMs=2900,pipeline=pipe(True)),
                               dict(epochMs=2900,pipeline=pipe(False))]
        r=evaluate(cap,p,0,150)
        self.assertFalse(r['ok'],r)
        self.assertEqual(r['status'],'unconfirmed',r)
        self.assertIn('publisherSamples.ambiguous-pre-stop-state@2900',r['missingEvidence'],r)
    def test_pre_stop_activity_requires_strict_stream_identity(self):
        cap,pub=fixture()
        def latest_pre_stop(p):
            latest=max(s['epochMs'] for s in p['publisherSamples']
                       if isinstance(s.get('epochMs'),(int,float))
                       and s['epochMs']<=p['stopRequestedEpochMs'])
            return [s for s in p['publisherSamples'] if s.get('epochMs')==latest]
        def check(name,p,status):
            r=evaluate(cap,p,0,150)
            self.results.append(dict(name='pre-stop-identity/'+name,
                                     expected=status=='confirmed',result=r))
            self.assertEqual(r['status'],status,(name,r))
            self.assertEqual(r['ok'],status=='confirmed',(name,r))
            return r
        # Identity defects on the sample that would prove pre-stop activity
        # must never be accepted as proof of the stopped stream's activity.
        for name,(change,reason) in {
            'port-zero':(dict(localUdpPort=0),'publisherSamples.localUdpPort-invalid'),
            'port-out-of-range':(dict(localUdpPort=70000),'publisherSamples.localUdpPort-invalid'),
            'port-missing':(dict(localUdpPort=None),'publisherSamples.localUdpPort-invalid'),
            'epoch-zero':(dict(publishEpoch='0'),'publisherSamples.publishEpoch-invalid'),
            'epoch-missing':(dict(publishEpoch=None),'publisherSamples.publishEpoch-invalid'),
        }.items():
            p=copy.deepcopy(pub)
            for s in latest_pre_stop(p):
                s['pipeline'].update(change)
            r=check(name,p,'unconfirmed')
            self.assertEqual(r['preStopSamples'][-1]['identity'],'unconfirmed',(name,r))
            self.assertIn(reason,r['missingEvidence'],(name,r))
            self.assertNotIn('publisher-not-active-before-stop',r['rejectedEvidence'],(name,r))
        # Top level and pipeline identity must agree with each other.
        for name,top,change in (
            ('epoch-conflict',dict(publishEpoch='1'),dict(publishEpoch='2')),
            ('port-conflict',dict(localUdpPort=50001),dict(localUdpPort=50002)),
        ):
            p=copy.deepcopy(pub)
            for s in latest_pre_stop(p):
                s.update(top)
                s['pipeline'].update(change)
            r=check(name,p,'unconfirmed')
            self.assertEqual(r['preStopSamples'][-1]['identity'],'unconfirmed',(name,r))
        # A top-level current generation with a pipeline other generation is
        # contradictory, not "another generation to be ignored".
        p=copy.deepcopy(pub)
        p['publisherSamples'].append(dict(epochMs=2950,publishEpoch='1',
            pipeline=dict(publisher=True,publishEpoch='2',localUdpPort=50001)))
        r=check('top-vs-pipeline-conflict',p,'unconfirmed')
        self.assertIn('publisherSamples.publishEpoch-conflict',r['missingEvidence'],r)
        # A newer strictly valid other stream does not prove the stopped stream
        # was active; the last pre-stop state is unconfirmed, never rejected.
        p=copy.deepcopy(pub)
        p['publisherSamples'].append(dict(epochMs=2950,
            pipeline=dict(publisher=True,publishEpoch='2',localUdpPort=50001)))
        r=check('newer-other-stream',p,'unconfirmed')
        self.assertEqual(r['preStopSamples'][-1]['identity'],'other',r)
        self.assertNotIn('publisher-not-active-before-stop',r['rejectedEvidence'],r)
        # A newer other stream that is not publishing is recorded only; it does
        # not accuse the stopped stream or invalidate a valid proof.
        p=copy.deepcopy(pub)
        p['publisherSamples'].append(dict(epochMs=2950,
            pipeline=dict(publisher=False,publishEpoch='2',localUdpPort=50001)))
        r=check('newer-inactive-other-stream',p,'confirmed')
        self.assertEqual(r['preStopSamples'][-1]['identity'],'other',r)
        # An older other stream is recorded but does not disturb a valid proof.
        p=copy.deepcopy(pub)
        p['publisherSamples'].append(dict(epochMs=2850,
            pipeline=dict(publisher=True,publishEpoch='2',localUdpPort=50001)))
        r=check('older-other-stream',p,'confirmed')
        self.assertEqual([(e['epochMs'],e['identity']) for e in r['preStopSamples']],
                         [(2850,'other'),(2900,'same')],r)
        self.assertEqual(r['preStopSampleCounts'],
                         {'same':1,'other':1,'unconfirmed':0,'unbound':0},r)
        # The current stream is inactive before the request: an active other
        # generation must not revive it, and the proof is rejected, not proven.
        for name,other in (('other-epoch-active',dict(publishEpoch='2')),
                           ('other-port-active',dict(localUdpPort=50002))):
            p=copy.deepcopy(pub)
            for s in latest_pre_stop(p):
                s['pipeline']['publisher']=False
            other_pipeline=dict(publisher=True,publishEpoch='1',localUdpPort=50001)
            other_pipeline.update(other)
            p['publisherSamples'].append(dict(epochMs=2990,pipeline=other_pipeline))
            r=check(name,p,'rejected')
            self.assertIn('publisher-not-active-before-stop',r['rejectedEvidence'],(name,r))
            self.assertFalse(r['publisherActiveBeforeRequestedStop'],(name,r))
            self.assertEqual([e['identity'] for e in r['preStopSamples'] if e['identity']=='other'],
                             ['other'],(name,r))
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--out');args=parser.parse_args()
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(StopProofTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    if args.out:
        with open(args.out,'x',encoding='utf-8') as output:json.dump(StopProofTests.results,output,indent=2)
    raise SystemExit(0 if result.wasSuccessful() else 1)
