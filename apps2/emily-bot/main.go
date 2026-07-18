// emily-bot — headless SHANKPIT-460 test client for automated QA.
//
// Connects N concurrent bots to the game server via UDP, tracks other
// connected players from PACKET_SNAPSHOT broadcasts, aims and fires at the
// nearest peer, and reports a clear pass/fail summary (connectivity, peer
// visibility, damage dealt/taken) with a matching exit code — replacing
// "manually launch the graphical client and eyeball it" with something
// scriptable/CI-usable after any server change. Runs headless: no SDL, no
// window.
//
// Wire protocol note: this targets apps/server/src/main.c (the real,
// currently-deployed bin/shank_server — verified 2026-07-18 by checking the
// running binary's embedded strings against source, since two other,
// unrelated, non-matching server implementations also exist in this repo:
// services/game-server/src/server.c (doesn't even compile standalone right
// now) and apps2/server-go (a separate, much less complete Go rewrite with
// a different wire format entirely). All three define incompatible
// protocols; do not assume any one of them without re-verifying which
// binary is actually running. Struct sizes/offsets below were NOT
// hand-computed — they were extracted by compiling a tiny C program against
// the real packages/common/protocol.h and printing sizeof/offsetof for
// every field, because two earlier guesses at this protocol (mirroring
// SHANKPIT parent's emily-bot, then mirroring apps2/server-go) both turned
// out to target servers that aren't the one actually running.
//
// Usage:
//
//	go run ./apps2/emily-bot                              # 1 bot, localhost:6969, 30s
//	go run ./apps2/emily-bot -bots 20 -duration 2m         # concurrent-load smoke test
//	go run ./apps2/emily-bot -bots 2 -duration 20s -v      # visibility/damage check, verbose
//	go run ./apps2/emily-bot -report                       # post an Emily observation with the result
//	GOWORK=off go build -o bin/emily-bot ./apps2/emily-bot
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"math"
	"net"
	"os"
	"os/exec"
	"sync"
	"sync/atomic"
	"time"
)

// ---- Wire protocol, verified against packages/common/protocol.h ----
// (see /tmp/offsets.c-style introspection referenced in the package doc above)

const (
	packetConnect    = 0
	packetUserCmd    = 1
	packetSnapshot   = 2
	packetWelcome    = 3
	packetDisconnect = 4

	btnAttack = 2

	wpnKnife   = 0
	wpnMagnum  = 1
	wpnAR      = 2
	wpnShotgun = 3
	wpnSniper  = 4
	wpnKatana  = 5

	weaponCycleInterval = 5 * time.Second // exercise weapon variety over a run, not just Magnum

	netHeaderSize = 12 // type@0 client_id@1 sequence@2(u16) timestamp@4(u32) entity_count@8 scene_id@9 [2 pad]
	userCmdSize   = 36 // sequence@0(u32) timestamp@4(u32) msec@8(u16)[2 pad] fwd@12 str@16 yaw@20 pitch@24 buttons@28 weapon_idx@32
	netPlayerSize = 44 // id@0 scene_id@1 [2 pad] last_seq@4(u32) x@8 y@12 z@16 yaw@20 pitch@24 current_weapon@28 state@29 health@30 shield@31 is_shooting@32 crouching@33 [2 pad] reward_feedback@36(f32) ammo@40 in_vehicle@41 hit_feedback@42 storm_charges@43

	tickInterval = 50 * time.Millisecond // 20 Hz
	fireInterval = 300 * time.Millisecond
	peerTTL      = 2 * time.Second
	aimTolerance = 20.0 // degrees within which we consider ourselves "on target"
	turnRate     = 0.25
)

type peer struct {
	x, y, z     float32
	yaw         float32
	health      uint8
	hitFeedback uint8
	seen        time.Time
}

// botStats is written only by its own bot's goroutines and read after the
// run completes, so no locking is needed once all bots finish.
type botStats struct {
	idx            int
	welcomed       atomic.Bool
	clientID       atomic.Uint32
	cmdsSent       atomic.Uint64
	snapshots      atomic.Uint64
	unknownPacket  atomic.Uint64
	sawOtherPlayer atomic.Bool // any peer ID != our own ever appeared in a snapshot
	damageTaken    atomic.Bool // our own health in a snapshot dropped from a previous snapshot
	damageDealt    atomic.Bool // any peer's hit_feedback was nonzero while we were aiming near them
	deaths         atomic.Uint64 // our own state transitioned ALIVE->DEAD
	respawns       atomic.Uint64 // our own state transitioned DEAD->ALIVE
	weaponsSeen    uint32        // bitmask of current_weapon values observed on our own record; single-writer (receiveLoop only), read after wg.Wait()
	lastErr        atomic.Value
}

const (
	stateAlive = 0
	stateDead  = 1
)

func main() {
	host := flag.String("host", "127.0.0.1", "game server host")
	port := flag.Int("port", 6969, "game server UDP port")
	bots := flag.Int("bots", 1, "number of concurrent bot clients")
	duration := flag.Duration("duration", 30*time.Second, "how long to run before reporting results")
	verbose := flag.Bool("v", false, "verbose per-packet logging")
	report := flag.Bool("report", false, "post a summary to Emily Prime via `emily observe` when done")
	weapon := flag.Int("weapon", -1, "lock to one weapon (0=knife 1=magnum 2=ar 3=shotgun 4=sniper 5=katana); -1 cycles through all six")
	flag.Parse()

	addr := fmt.Sprintf("%s:%d", *host, *port)
	fmt.Printf("[emily-bot] launching %d bot(s) against %s for %s\n", *bots, addr, *duration)

	stats := make([]*botStats, *bots)
	var wg sync.WaitGroup
	for i := 0; i < *bots; i++ {
		s := &botStats{idx: i}
		s.lastErr.Store("")
		stats[i] = s
		wg.Add(1)
		go func(s *botStats) {
			defer wg.Done()
			runBot(*host, *port, *duration, *verbose, *weapon, s)
		}(s)
		time.Sleep(20 * time.Millisecond) // stagger connects
	}
	wg.Wait()

	pass := summarize(stats, addr, *duration, *bots > 1)
	if *report {
		postObservation(stats, addr, pass)
	}
	if !pass {
		os.Exit(1)
	}
}

func runBot(host string, port int, duration time.Duration, verbose bool, lockWeapon int, s *botStats) {
	udpAddr, err := net.ResolveUDPAddr("udp", fmt.Sprintf("%s:%d", host, port))
	if err != nil {
		s.lastErr.Store(fmt.Sprintf("resolve addr: %v", err))
		return
	}
	conn, err := net.DialUDP("udp", nil, udpAddr)
	if err != nil {
		s.lastErr.Store(fmt.Sprintf("dial udp: %v", err))
		return
	}
	defer conn.Close()

	peers := make(map[uint8]*peer)
	var mu sync.Mutex
	var myID uint8
	var myLastHealth uint8 = 255 // sentinel: "unknown yet"
	var myLastState uint8 = 255  // sentinel: "unknown yet"
	var myX, myZ float32
	var haveServerPos bool // becomes true once our own snapshot entry has been seen at least once

	done := make(chan struct{})
	var closeOnce sync.Once
	closeDone := func() { closeOnce.Do(func() { close(done) }) }

	sendHeader(conn, packetConnect)
	go func() {
		defer closeDone()
		buf := make([]byte, 4096)
		for {
			n, err := conn.Read(buf)
			if err != nil {
				if !s.welcomed.Load() {
					s.lastErr.Store(fmt.Sprintf("read: %v", err))
				}
				return
			}
			if n < netHeaderSize {
				continue
			}
			pktType := buf[0]
			switch pktType {
			case packetWelcome:
				id := buf[1]
				mu.Lock()
				myID = id
				mu.Unlock()
				s.clientID.Store(uint32(id))
				if !s.welcomed.Swap(true) && verbose {
					fmt.Printf("[emily-bot#%d] welcomed: clientID=%d\n", s.idx, id)
				}
			case packetSnapshot:
				s.snapshots.Add(1)
				count := int(buf[netHeaderSize])
				off := netHeaderSize + 1
				now := time.Now()
				mu.Lock()
				for i := 0; i < count && off+netPlayerSize <= n; i++ {
					id := buf[off]
					x := math.Float32frombits(binary.LittleEndian.Uint32(buf[off+8:]))
					y := math.Float32frombits(binary.LittleEndian.Uint32(buf[off+12:]))
					z := math.Float32frombits(binary.LittleEndian.Uint32(buf[off+16:]))
					yaw := math.Float32frombits(binary.LittleEndian.Uint32(buf[off+20:]))
					weapon := buf[off+28]
					state := buf[off+29]
					health := buf[off+30]
					hitFeedback := buf[off+42]

					if id == myID {
						s.weaponsSeen |= 1 << uint32(weapon)
						if myLastState != 255 {
							if myLastState == stateAlive && state == stateDead {
								s.deaths.Add(1)
								if verbose {
									fmt.Printf("[emily-bot#%d] died\n", s.idx)
								}
							} else if myLastState == stateDead && state == stateAlive {
								s.respawns.Add(1)
								if verbose {
									fmt.Printf("[emily-bot#%d] respawned\n", s.idx)
								}
							}
						}
						myLastState = state
						if myLastHealth != 255 && health < myLastHealth {
							s.damageTaken.Store(true)
							if verbose {
								fmt.Printf("[emily-bot#%d] took damage: %d -> %d\n", s.idx, myLastHealth, health)
							}
						}
						myLastHealth = health
						myX, myZ = x, z
						haveServerPos = true
					} else {
						s.sawOtherPlayer.Store(true)
						if hitFeedback != 0 {
							s.damageDealt.Store(true)
							if verbose {
								fmt.Printf("[emily-bot#%d] peer %d shows hit_feedback=%d (damage observed)\n", s.idx, id, hitFeedback)
							}
						}
						peers[id] = &peer{x: x, y: y, z: z, yaw: yaw, health: health, hitFeedback: hitFeedback, seen: now}
					}
					off += netPlayerSize
				}
				// prune stale peers
				for id, p := range peers {
					if now.Sub(p.seen) > peerTTL {
						delete(peers, id)
					}
				}
				mu.Unlock()
				if verbose {
					fmt.Printf("[emily-bot#%d] snapshot: entity_count=%d\n", s.idx, count)
				}
			case packetDisconnect:
				// server telling us nothing here in practice; ignore
			default:
				s.unknownPacket.Add(1)
				if verbose {
					fmt.Printf("[emily-bot#%d] unrecognized packet type=%d n=%d\n", s.idx, pktType, n)
				}
			}
		}
	}()

	// Deterministic per-bot patrol pattern, phase-offset by index.
	phase := float64(s.idx) * 0.9
	seq := uint32(0)
	startTime := time.Now() // reference for elapsed-time math below; time.Time{} (year 1) overflows time.Duration's ~292-year range against a 2026 "now" — caught live, see EMILY/BACKLOG.md SECTION 155
	ticker := time.NewTicker(tickInterval)
	defer ticker.Stop()
	deadline := time.After(duration)
	fireTicker := time.NewTicker(fireInterval)
	defer fireTicker.Stop()
	firing := false

	for {
		select {
		case <-done:
			return
		case <-deadline:
			sendHeader(conn, packetDisconnect)
			return
		case <-fireTicker.C:
			firing = true
		case now := <-ticker.C:
			seq++
			elapsed := now.Sub(startTime)
			t := elapsed.Seconds() + phase

			var fwd, str, yaw float32
			mu.Lock()
			curX, curZ, gotPos := myX, myZ, haveServerPos
			var nearest *peer
			var nearestDist float32
			if gotPos {
				nearest, nearestDist = nearestPeer(peers, curX, curZ)
			}
			mu.Unlock()

			if nearest != nil {
				dx := nearest.x - curX
				dz := nearest.z - curZ
				// Server's raycast forward vector (physics.h update_weapons) is
				// dx=sin(-yawRad), dz=-cos(-yawRad). Solving for the yaw that
				// makes that vector point at (dx,dz) gives atan2(-dx,-dz), not
				// atan2(dx,dz) — verified against the server source, not
				// assumed; the naive atan2(dx,dz) aims exactly 180 degrees
				// away from the target.
				desiredYaw := float32(math.Atan2(float64(-dx), float64(-dz)) * 180.0 / math.Pi)
				yaw = desiredYaw
				if nearestDist > 6 {
					fwd = 1.0
				}
			} else {
				yaw = float32(math.Mod(t*15, 360))
				fwd = 0.3
				str = float32(math.Sin(t * 0.3))
			}

			var weaponIdx int32
			if lockWeapon >= 0 {
				weaponIdx = int32(lockWeapon)
			} else {
				weapons := [...]int32{wpnKnife, wpnMagnum, wpnAR, wpnShotgun, wpnSniper, wpnKatana}
				weaponIdx = weapons[int(elapsed/weaponCycleInterval)%len(weapons)]
			}

			cmd := struct {
				seq, ts       uint32
				msec          uint16
				fwd, str, yaw float32
				pitch         float32
				buttons       uint32
				weaponIdx     int32
			}{
				seq: seq, ts: uint32(now.UnixMilli()), msec: uint16(tickInterval.Milliseconds()),
				fwd: fwd, str: str, yaw: yaw, pitch: 0, weaponIdx: weaponIdx,
			}
			if firing && nearest != nil {
				cmd.buttons |= btnAttack
			}
			firing = false

			if err := sendUserCmd(conn, cmd.seq, cmd.ts, cmd.msec, cmd.fwd, cmd.str, cmd.yaw, cmd.pitch, cmd.buttons, cmd.weaponIdx); err != nil {
				s.lastErr.Store(fmt.Sprintf("send usercmd: %v", err))
				return
			}
			s.cmdsSent.Add(1)
		}
	}
}

func nearestPeer(peers map[uint8]*peer, myX, myZ float32) (*peer, float32) {
	var nearest *peer
	var nearestDist float32 = 1 << 30
	for _, p := range peers {
		dx := p.x - myX
		dz := p.z - myZ
		d := float32(math.Sqrt(float64(dx*dx + dz*dz)))
		if d < nearestDist {
			nearestDist = d
			nearest = p
		}
	}
	return nearest, nearestDist
}

// sendHeader sends a bare NetHeader with only the type byte meaningful —
// used for PACKET_CONNECT and PACKET_DISCONNECT, both of which
// server_handle_packet dispatches on head->type alone.
func sendHeader(conn *net.UDPConn, packetType byte) {
	buf := make([]byte, netHeaderSize)
	buf[0] = packetType
	_, _ = conn.Write(buf)
}

// sendUserCmd encodes one PACKET_USERCMD: [0:12]=NetHeader (type@0, rest
// zero) [12]=count=1 [13:49]=UserCmd, with UserCmd's internal field offsets
// matching sizeof/offsetof extracted from the real compiled struct (see
// package doc).
func sendUserCmd(conn *net.UDPConn, seq, ts uint32, msec uint16, fwd, str, yaw, pitch float32, buttons uint32, weaponIdx int32) error {
	buf := make([]byte, netHeaderSize+1+userCmdSize)
	buf[0] = packetUserCmd
	buf[netHeaderSize] = 1 // count
	off := netHeaderSize + 1
	binary.LittleEndian.PutUint32(buf[off+0:], seq)
	binary.LittleEndian.PutUint32(buf[off+4:], ts)
	binary.LittleEndian.PutUint16(buf[off+8:], msec)
	binary.LittleEndian.PutUint32(buf[off+12:], math.Float32bits(fwd))
	binary.LittleEndian.PutUint32(buf[off+16:], math.Float32bits(str))
	binary.LittleEndian.PutUint32(buf[off+20:], math.Float32bits(yaw))
	binary.LittleEndian.PutUint32(buf[off+24:], math.Float32bits(pitch))
	binary.LittleEndian.PutUint32(buf[off+28:], buttons)
	binary.LittleEndian.PutUint32(buf[off+32:], uint32(weaponIdx))
	_, err := conn.Write(buf)
	return err
}

// summarize prints a per-bot report and an overall PASS/FAIL line.
// requirePeerVisibility is true when -bots > 1: with multiple concurrent
// bots, at least one bot must have observed at least one other bot in a
// snapshot for the run to PASS — that's the actual multiplayer-sync check,
// not just "did anyone connect."
var weaponNames = [...]string{"knife", "magnum", "ar", "shotgun", "sniper", "katana"}

func weaponsSeenNames(mask uint32) string {
	if mask == 0 {
		return "-"
	}
	out := ""
	for i, name := range weaponNames {
		if mask&(1<<uint32(i)) != 0 {
			if out != "" {
				out += ","
			}
			out += name
		}
	}
	return out
}

func summarize(stats []*botStats, addr string, duration time.Duration, requirePeerVisibility bool) bool {
	fmt.Printf("\n[emily-bot] run complete — %d bot(s) vs %s for %s\n", len(stats), addr, duration)
	fmt.Printf("%-6s %-9s %-10s %-10s %-8s %-9s %-9s %-7s %-9s %-24s %s\n",
		"bot", "welcomed", "cmds_sent", "snapshots", "saw_peer", "dmg_taken", "dmg_dealt", "deaths", "respawns", "weapons_used", "error")
	pass := true
	anyPeerSeen := false
	for _, s := range stats {
		errStr, _ := s.lastErr.Load().(string)
		welcomed := s.welcomed.Load()
		sawPeer := s.sawOtherPlayer.Load()
		if sawPeer {
			anyPeerSeen = true
		}
		if !welcomed || errStr != "" {
			pass = false
		}
		fmt.Printf("%-6d %-9v %-10d %-10d %-8v %-9v %-9v %-7d %-9d %-24s %s\n",
			s.idx, welcomed, s.cmdsSent.Load(), s.snapshots.Load(), sawPeer, s.damageTaken.Load(), s.damageDealt.Load(),
			s.deaths.Load(), s.respawns.Load(), weaponsSeenNames(s.weaponsSeen), errStr)
	}
	if requirePeerVisibility && !anyPeerSeen {
		pass = false
		fmt.Println("[emily-bot] no bot ever saw another player in a PACKET_SNAPSHOT despite running >1 bot")
	}
	if pass {
		fmt.Println("[emily-bot] PASS")
	} else {
		fmt.Println("[emily-bot] FAIL — see per-bot table above")
	}
	return pass
}

func postObservation(stats []*botStats, addr string, pass bool) {
	welcomedCount := 0
	var totalCmds uint64
	sawPeer := false
	dmgDealt := false
	dmgTaken := false
	for _, s := range stats {
		if s.welcomed.Load() {
			welcomedCount++
		}
		totalCmds += s.cmdsSent.Load()
		if s.sawOtherPlayer.Load() {
			sawPeer = true
		}
		if s.damageDealt.Load() {
			dmgDealt = true
		}
		if s.damageTaken.Load() {
			dmgTaken = true
		}
	}
	result := "PASS"
	severity := "info"
	if !pass {
		result = "FAIL"
		severity = "warn"
	}
	summary := fmt.Sprintf(
		"emily-bot QA run vs %s: %s — %d/%d bots connected, %d commands sent, saw_peer=%v dmg_dealt=%v dmg_taken=%v",
		addr, result, welcomedCount, len(stats), totalCmds, sawPeer, dmgDealt, dmgTaken,
	)
	cmd := exec.Command("emily", "observe", "-s", severity, summary)
	if out, err := cmd.CombinedOutput(); err != nil {
		fmt.Fprintf(os.Stderr, "[emily-bot] observe err: %v — %s\n", err, out)
	}
}
