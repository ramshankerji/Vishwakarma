---
title: "Unit of Measurements"
weight: 11110
---
Only SI Units. We encourage use of SI units only. The whole software has been designed assuming mainly SI units. If your domain still uses Foot / Pound / Inches / Miles, god bless you. Please start using SI units. Be the change. Having said that, we do support these 2nd class units in specific domains.

Now, Let's talking about precision. Different domains of engineering have different precision requirements. For example: 

**Intermediate Scale:**

Concrete is creating using course aggregate varying from size 10 mm to 40 mm or even 80 mm in case of Dams. Placing concrete with sub-mm accuracy is impossible in real world. In case of structural steel, 1mm is considered a good precision. All the structural steel plate manufacturers provide standard plate thickness in multiple of mm. So overall for civil engineers working on localized structure, "mm" is a perfect unit.

Mechanical engineers generally deal with sum-mm plate thicknesses. However they don't really go too deep. No plates shall be specified in more than 3 decimal places, say 3.234mm thickness. So "mm" is good for mechanical engineers as well, when used in conjunction with few decimal places.

Electrical, Instrumentation and Architecture are also happy with mm scale units.

Accordingly <b>mm</b> is our default unit of choice.

**Large Scale:**

For country scale operations, "mm" is too small. For example: India measures 3,214 km from north to south and 2,933 km  from east to west. That would be 3214000000mm x 2933000000mm. So for National scale km with 3 to 5 decimal digits is the unit to go. GPS accuracy is ? m.

**Small Scale:**
This one belongs to Electronics engineers who work at Chip / Circuit levels. The current state of art (2025) chip design nodes are marketed using Angstrom units. i.e 1/10th of a nano meter. The electrical lines inside an iphone's circuit boards are of the order of ? micro meters. Hence for electronics engineers in this domain, either nano meter "nm" is a good unit. When we weill develop for this domain, nano meter will be the default. However if we restrict ourself to PCB design only, than even mm with 4 or 5 decimal place is good enough.

Coincidentally : 1 km = 1000,000 mm (i.e. 6 order of magnitude) and 1 mm = 1000,000 nm (i.e. 6 order of magnitude)
