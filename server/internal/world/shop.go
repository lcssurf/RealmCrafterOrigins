package world

// ShopItem describes one item available in an NPC shop.
type ShopItem struct {
	ItemID       uint16
	Name         string
	ItemType     uint8
	SlotType     uint8
	WeaponDamage int16
	ArmorLevel   int16
	BuyPrice     int32
	SellPrice    int32
}

var npcShops = map[string][]ShopItem{}

// RegisterShop registers a shop inventory for the given NPC name.
func RegisterShop(npcName string, items []ShopItem) {
	npcShops[npcName] = items
}

// GetShop returns the shop items for the given NPC name, nil if none.
func GetShop(npcName string) []ShopItem {
	return npcShops[npcName]
}

// FindShopItem searches all registered shops for an item with the given ID.
func FindShopItem(itemID uint16) *ShopItem {
	for _, items := range npcShops {
		for i := range items {
			if items[i].ItemID == itemID {
				return &items[i]
			}
		}
	}
	return nil
}
