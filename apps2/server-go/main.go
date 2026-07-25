// NOT THE REAL SERVER (S155-02, EMILY/BACKLOG.md): this implementation is fully superseded by
// apps/server/src/main.c and is not built, run, or referenced by any script, Makefile, or
// systemd unit in this repo. It exists only as unremoved history from before the C server
// consolidated. Do not extend or wire this up without first checking apps/server/src/main.c --
// a real engineer/agent lost real time re-discovering this the hard way once already. This file
// is kept pending a deliberate NORTHSTAR scoping pass on what specifically gets cut vs. kept
// (see this repo's own CLAUDE.md), not because it's still in use.
package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"net"
	"time"

	"dragonsnshit/packages2/common"
	"dragonsnshit/server/player"
	"dragonsnshit/server/store"
	"dragonsnshit/server/system"
)

type world struct{}

type rayResult struct {
	pos system.Vec3
}

func (r rayResult) Position() system.Vec3 { return r.pos }

func (w world) RayTrace(start, end system.Vec3) (player.RaycastResult, bool) {
	return rayResult{}, false
}

type shankPlayer struct {
	pos       system.Vec3
	eyeHeight float64
	world     world
}

func (p *shankPlayer) Position() system.Vec3 { return p.pos }
func (p *shankPlayer) EyeHeight() float64    { return p.eyeHeight }
func (p *shankPlayer) World() player.RaycastWorld {
	return p.world
}
func (p *shankPlayer) SendSound(name string, pos system.Vec3) {
	fmt.Printf("[sound] %s at %.2f %.2f %.2f\n", name, pos.X, pos.Y, pos.Z)
}

type voxelBlock struct {
	x       uint8
	y       uint8
	z       uint8
	blockID uint16
}

type chunkCoord struct {
	x int
	z int
}

type clientInfo struct {
	id            uint8
	lastVoxelSent time.Time
	chunkIndex    int
}

func main() {
	addr, err := net.ResolveUDPAddr("udp", ":6969")
	if err != nil {
		panic(err)
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		panic(err)
	}
	defer conn.Close()

	fmt.Println("Go backend listening on :6969")
	buf := make([]byte, 2048)
	p := &shankPlayer{pos: system.Vec3{}, eyeHeight: 1.62, world: world{}}
	clientStore := store.NewMemoryClientStore()
	clients := make(map[string]clientInfo)
	nextClientID := uint8(0)

	for {
		conn.SetReadDeadline(time.Now().Add(250 * time.Millisecond))
		n, remote, err := conn.ReadFromUDP(buf)
		if err != nil {
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				continue
			}
			fmt.Printf("read error: %v\n", err)
			continue
		}
		if n < 1 {
			continue
		}
		const netHeaderSize = 12
		const userCmdSize = 36
		switch buf[0] {
		case common.PacketConnect:
			info, ok := clients[remote.String()]
			if !ok {
				info = clientInfo{id: nextClientID}
				if nextClientID < 255 {
					nextClientID++
				}
				clients[remote.String()] = info
			}
			sendWelcome(conn, remote, info.id)
			sendVoxelPacket(conn, remote, info)
		case common.PacketUserCmd:
			if n < netHeaderSize+1+userCmdSize {
				continue
			}
			info, ok := clients[remote.String()]
			if !ok {
				info = clientInfo{id: nextClientID}
				if nextClientID < 255 {
					nextClientID++
				}
			}
			info = sendVoxelPacket(conn, remote, info)
			clients[remote.String()] = info
			count := int(buf[netHeaderSize])
			if count < 1 {
				continue
			}
			cmd := parseUserCmd(buf, netHeaderSize+1)
			clientStore.Upsert(remote.String(), cmd)
			if cmd.Buttons&common.BtnAttack != 0 {
				hit, pos, hitEntity := player.HandleShankFire(p, float64(cmd.Yaw), float64(cmd.Pitch), int(cmd.WeaponIdx))
				if hit {
					sendImpact(conn, remote, pos, hitEntity, 0)
				}
			}
			_ = remote
		}
	}
}

func sendWelcome(conn *net.UDPConn, remote *net.UDPAddr, id uint8) {
	payload := make([]byte, 12)
	payload[0] = common.PacketWelcome
	payload[1] = id
	binary.LittleEndian.PutUint16(payload[2:], 0)
	binary.LittleEndian.PutUint32(payload[4:], uint32(time.Now().UnixMilli()))
	payload[8] = 0
	_, _ = conn.WriteToUDP(payload, remote)
}

func sendVoxelPacket(conn *net.UDPConn, remote *net.UDPAddr, info clientInfo) clientInfo {
	now := time.Now()
	if now.Sub(info.lastVoxelSent) < 500*time.Millisecond {
		return info
	}
	chunks := nearbyChunks(0, 0, 1)
	if len(chunks) == 0 {
		return info
	}
	chunk := chunks[info.chunkIndex%len(chunks)]
	blocks := scanChunkForVoxelBlocks(chunk.x, chunk.z)

	headerSize := 16
	blockSize := 6
	payload := make([]byte, headerSize+len(blocks)*blockSize)
	payload[0] = common.PacketVoxelData
	binary.LittleEndian.PutUint32(payload[4:], uint32(chunk.x))
	binary.LittleEndian.PutUint32(payload[8:], uint32(chunk.z))
	binary.LittleEndian.PutUint16(payload[12:], uint16(len(blocks)))

	offset := headerSize
	for _, blk := range blocks {
		payload[offset] = blk.x
		payload[offset+1] = blk.y
		payload[offset+2] = blk.z
		payload[offset+3] = 0
		binary.LittleEndian.PutUint16(payload[offset+4:], blk.blockID)
		offset += blockSize
	}
	_, _ = conn.WriteToUDP(payload, remote)
	info.lastVoxelSent = now
	info.chunkIndex++
	return info
}

func sendImpact(conn *net.UDPConn, remote *net.UDPAddr, pos system.Vec3, hitEntity bool, blockID uint16) {
	payload := make([]byte, 20)
	payload[0] = common.PacketImpact
	if hitEntity {
		payload[1] = 1
	}
	binary.LittleEndian.PutUint32(payload[4:], math.Float32bits(float32(pos.X)))
	binary.LittleEndian.PutUint32(payload[8:], math.Float32bits(float32(pos.Y)))
	binary.LittleEndian.PutUint32(payload[12:], math.Float32bits(float32(pos.Z)))
	binary.LittleEndian.PutUint16(payload[16:], blockID)
	_, _ = conn.WriteToUDP(payload, remote)
}

func parseUserCmd(data []byte, offset int) common.UserCmd {
	off := offset
	cmd := common.UserCmd{}
	cmd.Sequence = binary.LittleEndian.Uint32(data[off:])
	off += 4
	cmd.Timestamp = binary.LittleEndian.Uint32(data[off:])
	off += 4
	cmd.Msec = binary.LittleEndian.Uint16(data[off:])
	off += 4
	cmd.Fwd = math.Float32frombits(binary.LittleEndian.Uint32(data[off:]))
	off += 4
	cmd.Str = math.Float32frombits(binary.LittleEndian.Uint32(data[off:]))
	off += 4
	cmd.Yaw = math.Float32frombits(binary.LittleEndian.Uint32(data[off:]))
	off += 4
	cmd.Pitch = math.Float32frombits(binary.LittleEndian.Uint32(data[off:]))
	off += 4
	cmd.Buttons = binary.LittleEndian.Uint32(data[off:])
	off += 4
	cmd.WeaponIdx = int32(binary.LittleEndian.Uint32(data[off:]))
	return cmd
}

const (
	chunkSize   = 16
	chunkHeight = 16
	logBlockID  = 17
	leafBlockID = 18
)

func nearbyChunks(centerX, centerZ, radius int) []chunkCoord {
	if radius < 0 {
		return nil
	}
	chunks := make([]chunkCoord, 0, (radius*2+1)*(radius*2+1))
	for dz := -radius; dz <= radius; dz++ {
		for dx := -radius; dx <= radius; dx++ {
			chunks = append(chunks, chunkCoord{x: centerX + dx, z: centerZ + dz})
		}
	}
	return chunks
}

func scanChunkForVoxelBlocks(chunkX, chunkZ int) []voxelBlock {
	blocks := make([]uint16, chunkSize*chunkHeight*chunkSize)
	for _, tree := range treeSeedsForChunk(chunkX, chunkZ) {
		placeTree(blocks, tree.x, tree.z, tree.baseY)
	}

	results := make([]voxelBlock, 0, 64)
	for y := 0; y < chunkHeight; y++ {
		for z := 0; z < chunkSize; z++ {
			for x := 0; x < chunkSize; x++ {
				blockID := blocks[chunkIndex(x, y, z)]
				if blockID != logBlockID && blockID != leafBlockID {
					continue
				}
				results = append(results, voxelBlock{
					x:       uint8(x),
					y:       uint8(y),
					z:       uint8(z),
					blockID: blockID,
				})
			}
		}
	}
	return results
}

type treeSeed struct {
	x     int
	z     int
	baseY int
}

func treeSeedsForChunk(chunkX, chunkZ int) []treeSeed {
	switch {
	case chunkX == 0 && chunkZ == 0:
		return []treeSeed{
			{x: 8, z: 8, baseY: 0},
			{x: 3, z: 12, baseY: 0},
		}
	case chunkX == 1 && chunkZ == 0:
		return []treeSeed{
			{x: 6, z: 5, baseY: 0},
		}
	case chunkX == -1 && chunkZ == -1:
		return []treeSeed{
			{x: 11, z: 4, baseY: 0},
		}
	default:
		return nil
	}
}

func placeTree(blocks []uint16, trunkX, trunkZ, baseY int) {
	for y := 0; y < 4; y++ {
		setBlock(blocks, trunkX, baseY+y, trunkZ, logBlockID)
	}
	leafY := baseY + 4
	for dz := -1; dz <= 1; dz++ {
		for dx := -1; dx <= 1; dx++ {
			setBlock(blocks, trunkX+dx, leafY, trunkZ+dz, leafBlockID)
		}
	}
	setBlock(blocks, trunkX, leafY+1, trunkZ, leafBlockID)
}

func setBlock(blocks []uint16, x, y, z int, blockID uint16) {
	if x < 0 || x >= chunkSize || y < 0 || y >= chunkHeight || z < 0 || z >= chunkSize {
		return
	}
	blocks[chunkIndex(x, y, z)] = blockID
}

func chunkIndex(x, y, z int) int {
	return (y*chunkSize+z)*chunkSize + x
}
